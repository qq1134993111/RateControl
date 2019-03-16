#pragma once
//#include <stdint.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <boost/atomic.hpp>


#include <boost/lockfree/queue.hpp>
#include <boost/typeof/typeof.hpp>


#define ACCESS_ONCE(x) (*(volatile BOOST_TYPEOF(x) *)&(x))

class RateControl;
class TokenBucket
{
public:
	enum  ErrorCode
	{
		kSuccess = 0,
		kWouldBlock,
		kFailure
	};

public:
	boost::int32_t Acquire(boost::uint32_t bytes)
	{
		while (TryAcquire(bytes) != static_cast<boost::int32_t>(ErrorCode::kSuccess))
		{
			if (ACCESS_ONCE(valid_))
			{
				boost::this_thread::yield();
			}
			else
			{
				return static_cast<boost::int32_t>(ErrorCode::kWouldBlock);
			}
		}

		return static_cast<boost::int32_t>(ErrorCode::kSuccess);
	}

	boost::int32_t TryAcquire(boost::uint32_t bytes)
	{
		boost::uint32_t token_num;
		do
		{
			token_num = token_num_;
			if (token_num < bytes)
			{
				return static_cast<boost::int32_t>(ErrorCode::kFailure);
			}

		} while (!token_num_.compare_exchange_weak(token_num, token_num - bytes));

		return static_cast<boost::int32_t>(ErrorCode::kSuccess);
	}

	void Release()
	{
		valid_ = false;
	}

	void SetCapacity(boost::uint32_t capacity)
	{
		capacity_ = capacity;
	}
private:
	TokenBucket()
	{
		rate_contorl_ = NULL;
		valid_ = false;
	}

	void AddToken(boost::uint64_t current_time)
	{
		const boost::uint64_t total_add = static_cast<boost::uint64_t>((current_time - last_time_)*rate_limit_micro_);
		if (total_add > history_token_)
		{
			const boost::uint32_t add = static_cast<boost::uint32_t>(total_add - history_token_);
			history_token_ = total_add;

			boost::uint32_t token_num;
			boost::uint32_t new_token;
			do
			{
				token_num = token_num_;
				new_token = token_num + add;
				if (new_token > capacity_)
				{
					new_token = capacity_;
					history_token_ = 0;
					last_time_ = current_time;
				}
			} while (!token_num_.compare_exchange_weak(token_num, new_token));
		}
	}
private:
	friend class RateControl;
	RateControl* rate_contorl_;

	bool valid_{ false };
	boost::uint32_t capacity_;//桶大小
	boost::atomic<boost::uint32_t> token_num_;//当前token数量
	boost::uint64_t last_time_;
	boost::uint64_t history_token_;
	double rate_limit_micro_;//每微妙的限制
};

class RateControl
{
public:

	struct Second {};
	struct MilliSecond {};
	struct MicroSecond {};

	template<typename Precision = Second>
	static TokenBucket* GetInstance(double rate_limit)
	{
		if (s_rate_control_ == NULL)
		{
			boost::mutex::scoped_lock lc(s_rate_mtx_);
			if (NULL == s_rate_control_)
			{
				s_rate_control_ = Create();
				assert(s_rate_control_ != NULL);
			}
		}

		const double rate_limite_micro_second = RateLimitConvert<Precision>(rate_limit);

		TokenBucket* token_bucket = s_rate_control_->NewTokenBucket();
		assert(token_bucket != NULL);
		token_bucket->rate_contorl_ = s_rate_control_;
		token_bucket->rate_limit_micro_ = rate_limite_micro_second;
		token_bucket->history_token_ = 0;
		token_bucket->token_num_ = 0;
		const boost::uint32_t rhythm_micro_second = static_cast<boost::uint32_t>(0.5 / rate_limite_micro_second);
		if (rhythm_micro_second < s_rate_control_->rhythm_micro_)
		{
			s_rate_control_->rhythm_micro_ = std::max(rhythm_micro_second, kMinRhythmMicro);
		}

		token_bucket->capacity_ = static_cast<boost::uint32_t>(s_rate_control_->rhythm_micro_*rate_limite_micro_second * 3);
		token_bucket->capacity_ = std::max(token_bucket->capacity_, (boost::uint32_t)3);
		token_bucket->valid_ = true;
		token_bucket->last_time_ = s_rate_control_->GetRealTime();

		s_rate_control_->token_buckets_.push(token_bucket);
		++s_rate_control_->token_buckets_size_;

		return token_bucket;

	}
	static void DestoryInstance()
	{
		boost::mutex::scoped_lock lc(s_rate_mtx_);
		if (NULL != s_rate_control_)
		{
			delete s_rate_control_;
			s_rate_control_ = NULL;
		}
	}
protected:
	static RateControl* Create()
	{
		RateControl* rate_control = new RateControl;
		rate_control->Start();
		return rate_control;
	}

	void Start()
	{
		rhythm_micro_ = kMaxRhythmMicro;
		current_time_ = boost::chrono::duration_cast<boost::chrono::microseconds>(boost::chrono::high_resolution_clock::now().time_since_epoch()).count();
		running_ = true;
		thread_ = boost::thread(&RateControl::WorkerThread, this);
	}

	void Stop()
	{
		running_ = false;
		if (thread_.joinable())
		{
			thread_.join();
		}

		TokenBucket* token_buckets = NULL;
		while (token_buckets_.pop(token_buckets))
		{
			--token_buckets_size_;
			delete token_buckets;
		}

		while (reuse_buckets_.pop(token_buckets))
		{
			delete token_buckets;
		}

	}

	boost::uint64_t GetRealTime()
	{
		const boost::uint64_t time_micro = boost::chrono::duration_cast<boost::chrono::microseconds>(boost::chrono::high_resolution_clock::now().time_since_epoch()).count();
		if (current_time_ < time_micro)
		{
			current_time_ = time_micro;
		}

		return current_time_;
	}

	boost::uint64_t GetCurrentTime() const
	{
		return  ACCESS_ONCE(current_time_);
	}

	void WorkerThread()
	{
		TokenBucket* token_bucket = NULL;
		while (ACCESS_ONCE(running_))
		{
			const boost::uint64_t start_time = GetRealTime();
			const boost::uint32_t length = token_buckets_size_;
			for (boost::uint32_t i = 0; i < length; i++)
			{
				if (!token_buckets_.pop(token_bucket))
				{
					break;
				}

				if (token_bucket->valid_)
				{
					token_bucket->AddToken(GetRealTime());
					token_buckets_.push(token_bucket);
				}
				else
				{
					reuse_buckets_.push(token_bucket);
					--token_buckets_size_;
				}

				const boost::uint64_t diff_time = GetRealTime() - start_time;
				if (rhythm_micro_ > diff_time)
				{
					boost::this_thread::sleep_for(boost::chrono::microseconds(rhythm_micro_ - diff_time));
				}

			}
		}
	}

	TokenBucket* NewTokenBucket()
	{
		TokenBucket* token = NULL;
		if (reuse_buckets_.pop(token))
		{
			return token;
		}
		return new TokenBucket;
	}

	template<typename Precision>
	static inline double RateLimitConvert(double rate_limit);
public:
	~RateControl()
	{
		Stop();
	}
private:
	RateControl()
	{
		running_ = false;
		token_buckets_size_ = 0;
	}
private:

	boost::thread thread_;
	bool running_;
	boost::uint32_t rhythm_micro_{ kMaxRhythmMicro };//循环检查周期,微妙
	boost::uint64_t current_time_;
	boost::lockfree::queue<TokenBucket*> token_buckets_;
	boost::atomic<boost::uint32_t> token_buckets_size_;
	boost::lockfree::queue<TokenBucket*> reuse_buckets_;


	static  boost::mutex  s_rate_mtx_;
	static RateControl* s_rate_control_;
	static const boost::uint32_t kMinRhythmMicro = 500;//500微妙
	static const boost::uint32_t kMaxRhythmMicro = 1000000;//1秒
};

template<>
inline double RateControl::RateLimitConvert<RateControl::Second>(double rate_limit)
{
	return rate_limit / 1000000;
}

template<>
inline double RateControl::RateLimitConvert<RateControl::MilliSecond>(double rate_limit)
{
	return rate_limit / 1000;
}

template<>
inline double RateControl::RateLimitConvert<RateControl::MicroSecond>(double rate_limit)
{
	return rate_limit;
}
#pragma once
#include <stdint.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <algorithm>

#include "concurrentqueue/concurrentqueue.h"

#define ACCESS_ONCE(x) (*(volatile decltype(x) *)&(x))

class RateControl;
class TokenBucket
{
public:
	enum class ErrorCode
	{
		kSuccess = 0,
		kWouldBlock,
		kFailure
	};

public:
	int32_t Acquire(uint32_t bytes)
	{
		while (TryAcquire(bytes) != static_cast<int32_t>(ErrorCode::kSuccess))
		{
			if (ACCESS_ONCE(valid_))
			{
				std::this_thread::yield();
			}
			else
			{
				return static_cast<int32_t>(ErrorCode::kWouldBlock);
			}
		}

		return static_cast<int32_t>(ErrorCode::kSuccess);
	}

	int32_t TryAcquire(uint32_t bytes)
	{
		uint32_t token_num;
		do
		{
			token_num = token_num_;
			if (token_num < bytes)
			{
				return static_cast<int32_t>(ErrorCode::kFailure);
			}

		} while (!std::atomic_compare_exchange_weak(&token_num_, &token_num, token_num - bytes));

		return static_cast<int32_t>(ErrorCode::kSuccess);
	}

	void Release()
	{
		valid_ = false;
	}

	void SetCapacity(uint32_t capacity)
	{
		capacity_ = capacity;
	}
private:
	TokenBucket() = default;

	void AddToken(uint64_t current_time)
	{
		const uint64_t total_add = static_cast<uint64_t>((current_time - last_time_) * rate_limit_micro_);
		if (total_add != 0 && total_add >= history_token_)//���ٺ��ϴ����ӵ�����һ����ŷ���Ͱ��,��������ÿ�ζ�����.
		{
			const uint32_t add = static_cast<uint32_t>(total_add - history_token_);
			history_token_ = total_add;

			uint32_t token_num;
			uint32_t new_token;
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
				//ԭ�ӵرȽ�token_num_��token_num��ֵ,������,����new_token�滻token_num_������token_num_�е�ֵ���ؽ�token_num.���ɹ��ظ�����token_num_�򷵻�true,����Ϊfalse.
			} while (!std::atomic_compare_exchange_weak(&token_num_, &token_num, new_token));
			
		}
	}
private:
	friend class RateControl;
	RateControl* rate_contorl_ = nullptr;

	bool valid_{ false };//�Ƿ���Ч
	uint32_t capacity_;//Ͱ��С
	std::atomic<uint32_t> token_num_;//��ǰtoken����
	uint64_t last_time_;//������ʱ��
	uint64_t history_token_;
	double rate_limit_micro_;//����,ÿ΢�����������
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
		if (s_rate_control_ == nullptr)
		{
			std::unique_lock<std::mutex> lc(s_rate_mtx_);
			if (nullptr == s_rate_control_)
			{
				s_rate_control_ = Create();
				assert(s_rate_control_ != nullptr);
			}
		}

		//��������,ÿ΢��ķ��͸���.����ÿ�뷢��100��,��ôÿ΢�����100/1000000����
		const double rate_limite_micro_second = RateLimitConvert<Precision>(rate_limit);

		TokenBucket* token_bucket = s_rate_control_->NewTokenBucket();
		assert(token_bucket != nullptr);
		token_bucket->rate_contorl_ = s_rate_control_;
		token_bucket->rate_limit_micro_ = rate_limite_micro_second;
		token_bucket->history_token_ = 0;
		token_bucket->token_num_ = 0;
		//�����������,�߳�ÿ������΢�����¸���ÿ��Ͱ�е���������
		//����ÿ�����1000��,��ôÿ����(1/1000��)���Ը���һ������,Ҳ����1000΢�����1���� ��������Ϊ1/����,��1/1000.
		const uint32_t rhythm_micro_second = static_cast<uint32_t>(0.5 / rate_limite_micro_second); //���ø������ڱ�ƽ���ĸ������ڸ�Сһ��
		if (rhythm_micro_second < s_rate_control_->rhythm_micro_)
		{
			s_rate_control_->rhythm_micro_ = std::max(rhythm_micro_second, kMinRhythmMicro);//������ʵĸ�������,��С�ĸ�������ΪkMinRhythmMicro.
		}

		//capacity_����Ͱ������,����ͻ������.
		token_bucket->capacity_ = static_cast<uint32_t>(s_rate_control_->rhythm_micro_ * rate_limite_micro_second * 3);
		token_bucket->capacity_ = std::max(token_bucket->capacity_, (uint32_t)3);
		token_bucket->valid_ = true;
		token_bucket->last_time_ = s_rate_control_->GetRealTime();

		s_rate_control_->token_buckets_.enqueue(token_bucket);

		return token_bucket;

	}
	static void DestoryInstance()
	{
		std::unique_lock<std::mutex> lc(s_rate_mtx_);
		if (nullptr != s_rate_control_)
		{
			delete s_rate_control_;
			s_rate_control_ = nullptr;
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
		current_time_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
		running_ = true;
		thread_ = std::thread(&RateControl::WorkerThread, this);
	}

	void Stop()
	{
		running_ = false;
		if (thread_.joinable())
		{
			thread_.join();
		}

		TokenBucket* token_buckets = nullptr;
		while (token_buckets_.try_dequeue(token_buckets))
		{
			delete token_buckets;
		}

		while (reuse_buckets_.try_dequeue(token_buckets))
		{
			delete token_buckets;
		}

	}

	uint64_t GetRealTime()
	{
		const uint64_t time_micro = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
		if (current_time_ < time_micro)
		{
			current_time_ = time_micro;
		}

		return current_time_;
	}

	void WorkerThread()
	{
		TokenBucket* token_bucket = nullptr;
		while (ACCESS_ONCE(running_))
		{
			const uint64_t start_time = GetRealTime();

			const size_t length = token_buckets_.size_approx();
			for (size_t i = 0; i < length; i++)
			{
				if (!token_buckets_.try_dequeue(token_bucket))
				{
					break;
				}

				if (token_bucket->valid_)
				{
					token_bucket->AddToken(GetRealTime());
					token_buckets_.enqueue(token_bucket);
				}
				else
				{
					reuse_buckets_.enqueue(token_bucket);
				}
			}

			const uint64_t diff_time = GetRealTime() - start_time;
			if (rhythm_micro_ > diff_time)//�������һ�ֵ�ʱ��С�ڸ�������,ʣ�µ�ʱ�����Ϣ
			{
				std::this_thread::sleep_for(std::chrono::microseconds(rhythm_micro_ - diff_time));
			}
		}
	}

	TokenBucket* NewTokenBucket()
	{
		TokenBucket* token = nullptr;
		if (reuse_buckets_.try_dequeue(token))
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
	RateControl() {}
private:

	std::thread thread_;
	bool running_;
	uint32_t rhythm_micro_{ kMaxRhythmMicro };//ѭ���������,΢��
	uint64_t current_time_;
	moodycamel::ConcurrentQueue<TokenBucket*> token_buckets_;
	moodycamel::ConcurrentQueue<TokenBucket*> reuse_buckets_;
	static std::mutex s_rate_mtx_;
	static RateControl* s_rate_control_;
	static const uint32_t kMinRhythmMicro = 500;//500΢��
	static const uint32_t kMaxRhythmMicro = 1000000;//1��
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
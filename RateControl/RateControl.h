#pragma once
#include <stdint.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include "concurrentqueue.h"

class RateControl;
class TokenBucket
{
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
			if (valid_)
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
	TokenBucket() {}

	void AddToken(uint64_t current_time)
	{
		const uint64_t total_add = (current_time - last_time_micro)*rate_limit_micro;
		if (total_add > history_token_)
		{
			const uint64_t add = total_add - history_token_;
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
					last_time_micro = current_time;
				}
			} while (!std::atomic_compare_exchange_weak(&token_num_, &token_num, new_token));
		}
	}
private:
	friend class RateControl;
	RateControl* rate_contorl_ = nullptr;

	std::atomic<bool> valid_{ false };
	std::atomic<uint32_t> capacity_;//桶大小
	std::atomic<uint32_t> token_num_;//当前token数量
	uint64_t last_time_micro;
	uint64_t history_token_;
	double rate_limit_micro;//每微妙的限制
};

class RateControl
{
public:
	static TokenBucket* GetInstance(double rate_limite)//每秒的限定
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

		const double rate_limite_micro_second = rate_limite / 1000000;

		TokenBucket* token_bucket = s_rate_control_->NewTokenBucket();
		assert(token_bucket != nullptr);
		token_bucket->rate_contorl_ = s_rate_control_;
		token_bucket->rate_limit_micro = rate_limite_micro_second;
		token_bucket->history_token_ = 0;
		token_bucket->token_num_ = 0;
		const uint32_t rhythm_micro_second = static_cast<uint32_t>(0.5 / rate_limite_micro_second);
		if (rhythm_micro_second < s_rate_control_->rhythm_micro_)
		{
			s_rate_control_->rhythm_micro_ = std::max(rhythm_micro_second, kMinRhythmMicro);
		}
		token_bucket->capacity_ = rate_limite * 2;
		token_bucket->valid_ = true;
		token_bucket->last_time_micro = s_rate_control_->GetRealTime();

		s_rate_control_->token_buckets_.enqueue(token_bucket);

		return token_bucket;

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
		current_time_micro_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
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
		if (current_time_micro_ < time_micro)
		{
			current_time_micro_ = time_micro;
		}

		return current_time_micro_;
	}

	uint64_t GetCurrentTime() const
	{
		return current_time_micro_;
	}

	void WorkerThread()
	{
		TokenBucket* token_bucket = nullptr;
		while (running_)
		{
			const uint64_t start_time = GetRealTime();
			const uint32_t length = token_buckets_.size_approx();
			for (auto i = 0; i < length; i++)
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

				const uint64_t diff_time = GetRealTime() - start_time;
				if (rhythm_micro_ > diff_time)
				{
					std::this_thread::sleep_for(std::chrono::microseconds(rhythm_micro_ - diff_time));
				}

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
public:
	~RateControl()
	{
		Stop();
	}
private:
	RateControl() {}
private:

	std::thread thread_;
	std::atomic<bool> running_;
	std::atomic<uint32_t> rhythm_micro_{ kMaxRhythmMicro };//循环检查周期,微妙
	std::atomic<uint64_t> current_time_micro_;
	moodycamel::ConcurrentQueue<TokenBucket*> token_buckets_;
	moodycamel::ConcurrentQueue<TokenBucket*> reuse_buckets_;
	static std::mutex s_rate_mtx_;
	static RateControl* s_rate_control_;
	static const uint32_t kMinRhythmMicro = 500;//500微妙
	static const uint32_t kMaxRhythmMicro = 1000000;//1秒
};


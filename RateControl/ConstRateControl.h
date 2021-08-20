#pragma once
#include<stdint.h>
#include<vector>
#include<chrono>
#include<mutex>

class ConstRateControl
{
public:
	ConstRateControl() {}
	~ConstRateControl() {}

	template <class _Rep, class _Period>
	void SetRate(uint32_t count, std::chrono::duration<_Rep, _Period> duration)
	{
		v_tp_.resize(count);
		rate_count_ = count;
		nano_second_ = duration;
		head_ = tail_ = 0;
	}

	bool TryAcquire()
	{
		auto now = std::chrono::high_resolution_clock::now();


		if (head_ != tail_ + rate_count_)
		{
			v_tp_[head_ % rate_count_] = now;
			head_++;

			return true;
		}

		bool valid = false;

		for (; tail_ != head_; tail_++)
		{
			if (std::chrono::duration_cast<std::chrono::nanoseconds>(now - v_tp_[tail_ % rate_count_]).count() < nano_second_.count())
			{
				break;
			}
			else
			{
				valid = true;
			}

		}

		if (valid)
		{
			v_tp_[head_ % rate_count_] = now;
			head_++;
		}

		return valid;

	}

	bool TryAcquire(uint32_t count)
	{
		if (count > rate_count_)
		{
			return false;
		}

		auto now = std::chrono::high_resolution_clock::now();


		if (head_ + count <= tail_ + rate_count_)
		{
			auto end = head_ + count;
			while (head_ < end)
			{
				v_tp_[head_ % rate_count_] = now;
				head_++;
			}
			return true;
		}

		bool valid = false;

		for (uint32_t times = 0; tail_ != head_; tail_++)
		{
			if (std::chrono::duration_cast<std::chrono::nanoseconds>(now - v_tp_[tail_ % rate_count_]).count() < nano_second_.count())
			{
				break;
			}
			else
			{
				times++;
				if (times >= count)
				{
					valid = true;
				}
			}

		}

		if (valid)
		{
			auto end = head_ + count;
			while (head_ < end)
			{
				v_tp_[head_ % rate_count_] = now;
				head_++;
			}
		}

		return valid;

	}
private:
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> v_tp_;
	uint32_t rate_count_ = 0;
	uint64_t head_ = 0;
	uint64_t tail_ = 0;
	std::chrono::nanoseconds nano_second_;
};


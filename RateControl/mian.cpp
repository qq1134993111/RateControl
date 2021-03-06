// RateControl.cpp: 定义控制台应用程序的入口点。
//

#include <stdio.h>
#include "RateControl.h"
#include "ConstRateControl.h"
#include<ctime>
#include<iostream>
#include<iomanip>

void OutputNowTimestamp(std::ostream& stream)
{
	namespace chrono = std::chrono;

	// Because c-style date&time utilities don't support microsecond precison,
	// we have to handle it on our own.
	auto time_now = chrono::system_clock::now();
	auto duration_in_ms = chrono::duration_cast<chrono::milliseconds>(time_now.time_since_epoch());
	auto ms_part = duration_in_ms - chrono::duration_cast<chrono::seconds>(duration_in_ms);

	tm local_time_now;
	time_t raw_time = chrono::system_clock::to_time_t(time_now);
	_localtime64_s(&local_time_now, &raw_time);
	stream << std::put_time(&local_time_now, "%Y%m%d %H:%M:%S,")
		<< std::setfill('0') << std::setw(3) << ms_part.count();
}

int main()
{
	for (int j = 0; j < 3; j++)
	{
		TokenBucket* token = RateControl::GetInstance(2);
		token->SetCapacity(2 * 2);

		for (int i = 0; i < 10; i++)
		{
			if (token->Acquire(1) == 0)
			{
				OutputNowTimestamp(std::cout);
				printf("\t\ti=%d\n", i);
			}
		}

		token->Release();
	}


	RateControl::DestoryInstance();


	ConstRateControl ctrl;
	ctrl.SetRate(10, std::chrono::seconds(1));
	for (size_t i = 0; true; i++)
	{
		if (ctrl.TryAcquire(2))
		{

			OutputNowTimestamp(std::cout);
			std::cout << "\t\ti=" << i << "\n";
			static int t = 0;
			if (++t >= 30)
			{
				break;
			}
		}

	}
	std::cout << "Hello World!\n";


	return 0;
}


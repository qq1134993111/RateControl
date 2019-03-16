// RateControl.cpp: 定义控制台应用程序的入口点。
//

#include <stdio.h>
#include "RateControl.h"

int main()
{
	for (int j = 0 ; j < 3; j++)
	{
		TokenBucket* token = RateControl::GetInstance(2);
		token->SetCapacity(2 * 2);

		for (int i = 0; i < 10; i++)
		{
			if (token->Acquire(1) == 0)
			{
				printf("i=%d\n", i);
			}
		}

		token->Release();
	}


	RateControl::DestoryInstance();

    return 0;
}


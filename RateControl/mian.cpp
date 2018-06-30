// RateControl.cpp: 定义控制台应用程序的入口点。
//

#include <stdio.h>
#include "RateControl.h"

int main()
{
	TokenBucket* token=RateControl::GetInstance(100);
	
	for (int i=0;i<100;i++)
	{
		if (token->Acquire(1)==0)
		{
			printf("i=%d\n",i);
		}
	}
    return 0;
}


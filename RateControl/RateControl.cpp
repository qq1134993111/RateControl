#include "RateControl.h"

std::mutex RateControl::s_rate_mtx_;
RateControl* RateControl::s_rate_control_ = nullptr;

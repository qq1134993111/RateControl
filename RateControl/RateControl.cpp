#include "RateControl.h"

boost::mutex RateControl::s_rate_mtx_;
RateControl* RateControl::s_rate_control_ = nullptr;

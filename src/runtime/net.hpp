// SPDX-License-Identifier: MIT

#pragma once

#include <boost/corosio.hpp>

namespace liquid::runtime::net {

using io_context = boost::corosio::io_context;
using io_context_options = boost::corosio::io_context_options;

}  // namespace liquid::runtime::net

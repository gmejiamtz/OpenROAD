// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#pragma once

namespace cms {
class TritonCTS;
}

namespace ord {

class OpenRoad;

cms::TritonCTS* makeTritonCts();

void initTritonCts(OpenRoad* openroad);

void deleteTritonCts(cms::TritonCTS* tritoncts);

}  // namespace ord

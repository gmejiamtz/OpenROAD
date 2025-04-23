// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022-2025, The OpenROAD Authors

%{
#include "cms/TritonCTS.h"
#include "CtsOptions.h"
#include "TechChar.h"
#include "ord/OpenRoad.hh"

using namespace cms;
%}

%include "stdint.i"

%include "../../Exception-py.i"

%include <std_string.i>
%include <std_vector.i>

%ignore cms::CtsOptions::setObserver;
%ignore cms::CtsOptions::getObserver;

%include "CtsOptions.h"
%include "cms/TritonCTS.h"

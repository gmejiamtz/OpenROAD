// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "cms/MakeTritoncts.h"

#include "CtsOptions.h"
#include "cms/TritonCTS.h"
#include "odb/db.h"
#include "ord/OpenRoad.hh"
#include "utl/decode.h"

namespace cms {
// Tcl files encoded into strings.
extern const char* cms_tcl_inits[];
}  // namespace cms

extern "C" {
extern int Cms_Init(Tcl_Interp* interp);
}

namespace ord {

cms::TritonCTS* makeTritonCts()
{
  return new cms::TritonCTS();
}

void initTritonCts(OpenRoad* openroad)
{
  Tcl_Interp* tcl_interp = openroad->tclInterp();
  // Define swig TCL commands.
  Cms_Init(tcl_interp);
  utl::evalTclInit(tcl_interp, cms::cms_tcl_inits);
  openroad->getTritonCts()->init(openroad->getLogger(),
                                 openroad->getDb(),
                                 openroad->getDbNetwork(),
                                 openroad->getSta(),
                                 openroad->getSteinerTreeBuilder(),
                                 openroad->getResizer());
}

void deleteTritonCts(cms::TritonCTS* tritoncts)
{
  delete tritoncts;
}

}  // namespace ord

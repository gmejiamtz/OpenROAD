// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

#include "dpo/Optdp.h"

#include <odb/db.h>

#include <algorithm>
#include <boost/format.hpp>
#include <cfloat>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "dpl/Opendp.h"
#include "odb/util.h"
#include "ord/OpenRoad.hh"  // closestPtInRect
#include "utl/Logger.h"

// My stuff.
#include "architecture.h"
#include "detailed.h"
#include "detailed_manager.h"
#include "dpl/Grid.h"
#include "dpl/Objects.h"
#include "dpl/Padding.h"
#include "dpl/PlacementDRC.h"
#include "legalize_shift.h"
#include "network.h"
#include "router.h"
#include "symmetry.h"

namespace dpo {

using utl::DPO;

using dpl::Master;
using odb::dbBlock;
using odb::dbBlockage;
using odb::dbBox;
using odb::dbBTerm;
using odb::dbInst;
using odb::dbITerm;
using odb::dbMaster;
using odb::dbMPin;
using odb::dbMTerm;
using odb::dbNet;
using odb::dbOrientType;
using odb::dbRegion;
using odb::dbRow;
using odb::dbSBox;
using odb::dbSet;
using odb::dbSigType;
using odb::dbSite;
using odb::dbSWire;
using odb::dbTechLayer;
using odb::dbWireType;
using odb::Rect;

////////////////////////////////////////////////////////////////
void Optdp::init(odb::dbDatabase* db, utl::Logger* logger, dpl::Opendp* opendp)
{
  db_ = db;
  logger_ = logger;
  opendp_ = opendp;
}

////////////////////////////////////////////////////////////////
void Optdp::improvePlacement(const int seed,
                             const int max_displacement_x,
                             const int max_displacement_y)
{
  logger_->report("Detailed placement improvement.");

  odb::WireLengthEvaluator eval(db_->getChip()->getBlock());
  const int64_t hpwlBefore = eval.hpwl();

  if (hpwlBefore == 0) {
    logger_->report("Skipping detailed improvement since hpwl is zero.");
    return;
  }

  // Get needed information from DB.
  import();

  const bool disallow_one_site_gaps = !odb::hasOneSiteMaster(db_);

  // A manager to track cells.
  dpo::DetailedMgr mgr(arch_, network_, routeinfo_, grid_, drc_engine_);
  mgr.setLogger(logger_);
  // Various settings.
  mgr.setSeed(seed);
  mgr.setMaxDisplacement(max_displacement_x, max_displacement_y);
  mgr.setDisallowOneSiteGaps(disallow_one_site_gaps);

  // Legalization.  Doesn't particularly do much.  It only
  // populates the data structures required for detailed
  // improvement.  If it errors or prints a warning when
  // given a legal placement, that likely means there is
  // a bug in my code somewhere.
  dpo::ShiftLegalizer lg;
  lg.legalize(mgr);

  // Detailed improvement.  Runs through a number of different
  // optimizations aimed at wirelength improvement.  The last
  // call to the random improver can be set to consider things
  // like density, displacement, etc. in addition to wirelength.
  // Everything done through a script string.

  dpo::DetailedParams dtParams;
  dtParams.script_ = "";
  // Maximum independent set matching.
  dtParams.script_ += "mis -p 10 -t 0.005;";
  // Global swaps.
  dtParams.script_ += "gs -p 10 -t 0.005;";
  // Vertical swaps.
  dtParams.script_ += "vs -p 10 -t 0.005;";
  // Small reordering.
  dtParams.script_ += "ro -p 10 -t 0.005;";
  // Random moves and swaps with hpwl as a cost function.  Use
  // random moves and hpwl objective right now.
  dtParams.script_ += "default -p 5 -f 20 -gen rng -obj hpwl -cost (hpwl);";

  if (disallow_one_site_gaps) {
    dtParams.script_ += "disallow_one_site_gaps;";
  }

  // Run the script.
  dpo::Detailed dt(dtParams);
  dt.improve(mgr);

  // Write solution back.
  updateDbInstLocations();

  // Get final hpwl.
  const int64_t hpwlAfter = eval.hpwl();

  // Cleanup.
  delete network_;
  delete arch_;
  delete routeinfo_;
  delete drc_engine_;

  const double dbu_micron = db_->getTech()->getDbUnitsPerMicron();

  // Statistics.
  logger_->report("Detailed Improvement Results");
  logger_->report("------------------------------------------");
  logger_->report("Original HPWL         {:10.1f} u", hpwlBefore / dbu_micron);
  logger_->report("Final HPWL            {:10.1f} u", hpwlAfter / dbu_micron);
  const double hpwl_delta = (hpwlAfter - hpwlBefore) / (double) hpwlBefore;
  logger_->report("Delta HPWL            {:10.1f} %", hpwl_delta * 100);
  logger_->report("");
}

////////////////////////////////////////////////////////////////
void Optdp::import()
{
  logger_->report("Importing netlist into detailed improver.");

  network_ = new Network;
  arch_ = new Architecture;
  routeinfo_ = new RoutingParams;
  grid_ = new Grid;

  // createLayerMap(); // Does nothing right now.
  // createNdrMap(); // Does nothing right now.
  setupMasterPowers();  // Call prior to network and architecture creation.
  initPlacementDRC();
  createNetwork();       // Create network; _MUST_ do before architecture.
  createArchitecture();  // Create architecture.
  // createRouteInformation(); // Does nothing right now.
  createGrid();
  initPadding();  // Need to do after network creation.
  // setUpNdrRules(); // Does nothing right now.
  setUpPlacementGroups();  // Regions.
}

////////////////////////////////////////////////////////////////
void Optdp::updateDbInstLocations()
{
  for (dbInst* inst : db_->getChip()->getBlock()->getInsts()) {
    if (!inst->getMaster()->isCoreAutoPlaceable() || inst->isFixed()) {
      continue;
    }

    const auto it_n = instMap_.find(inst);
    if (it_n != instMap_.end()) {
      const Node* nd = it_n->second;

      const int y = nd->getBottom().v + grid_->getCore().yMin();
      const int x = nd->getLeft().v + grid_->getCore().xMin();

      if (inst->getOrient() != nd->getOrient()) {
        inst->setOrient(nd->getOrient());
      }
      int inst_x, inst_y;
      inst->getLocation(inst_x, inst_y);
      if (x != inst_x || y != inst_y) {
        inst->setLocation(x, y);
      }
    }
  }
}

////////////////////////////////////////////////////////////////
void Optdp::initPlacementDRC()
{
  drc_engine_ = new PlacementDRC(grid_, db_->getTech());
}

////////////////////////////////////////////////////////////////
void Optdp::initPadding()
{
  // Grab information from OpenDP.

  // Need to turn on padding.
  arch_->setUsePadding(true);

  // Create and edge type for each amount of padding.  This
  // can be done by querying OpenDP.
  odb::dbSite* site = nullptr;
  for (auto* row : db_->getChip()->getBlock()->getRows()) {
    if (row->getSite()->getClass() != odb::dbSiteClass::PAD) {
      site = row->getSite();
      break;
    }
  }
  if (site == nullptr) {
    return;
  }
  const int siteWidth = site->getWidth();

  for (dbInst* inst : db_->getChip()->getBlock()->getInsts()) {
    const auto it_n = instMap_.find(inst);
    if (it_n != instMap_.end()) {
      Node* ndi = it_n->second;
      const int leftPadding = opendp_->padLeft(inst);
      const int rightPadding = opendp_->padRight(inst);
      arch_->addCellPadding(
          ndi, leftPadding * siteWidth, rightPadding * siteWidth);
    }
  }
}

////////////////////////////////////////////////////////////////
void Optdp::createLayerMap()
{
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::createNdrMap()
{
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::createRouteInformation()
{
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::setUpNdrRules()
{
  // Relates to pin blockages, etc. Not used rignt now.
  ;
}
////////////////////////////////////////////////////////////////
void Optdp::setupMasterPowers()
{
  // Need to try and figure out which voltages are on the
  // top and bottom of the masters/insts in order to set
  // stuff up for proper row alignment of multi-height
  // cells.  What I do it look at the individual ports
  // (MTerms) and see which ones correspond to POWER and
  // GROUND and then figure out which one is on top and
  // which one is on bottom.  I also record the layers
  // and use that information later when setting up the
  // row powers.
  dbBlock* block = db_->getChip()->getBlock();
  std::vector<dbMaster*> masters;
  block->getMasters(masters);

  for (dbMaster* master : masters) {
    int maxPwr = std::numeric_limits<int>::min();
    int minPwr = std::numeric_limits<int>::max();
    int maxGnd = std::numeric_limits<int>::min();
    int minGnd = std::numeric_limits<int>::max();

    bool isVdd = false;
    bool isGnd = false;
    for (dbMTerm* mterm : master->getMTerms()) {
      if (mterm->getSigType() == dbSigType::POWER) {
        isVdd = true;
        for (dbMPin* mpin : mterm->getMPins()) {
          // Geometry or box?
          const int y = mpin->getBBox().yCenter();
          minPwr = std::min(minPwr, y);
          maxPwr = std::max(maxPwr, y);

          for (dbBox* mbox : mpin->getGeometry()) {
            dbTechLayer* layer = mbox->getTechLayer();
            pwrLayers_.insert(layer);
          }
        }
      } else if (mterm->getSigType() == dbSigType::GROUND) {
        isGnd = true;
        for (dbMPin* mpin : mterm->getMPins()) {
          // Geometry or box?
          const int y = mpin->getBBox().yCenter();
          minGnd = std::min(minGnd, y);
          maxGnd = std::max(maxGnd, y);

          for (dbBox* mbox : mpin->getGeometry()) {
            dbTechLayer* layer = mbox->getTechLayer();
            gndLayers_.insert(layer);
          }
        }
      }
    }
    int topPwr = Architecture::Row::Power_UNK;
    int botPwr = Architecture::Row::Power_UNK;
    if (isVdd && isGnd) {
      topPwr = (maxPwr > maxGnd) ? Architecture::Row::Power_VDD
                                 : Architecture::Row::Power_VSS;
      botPwr = (minPwr < minGnd) ? Architecture::Row::Power_VDD
                                 : Architecture::Row::Power_VSS;
    }

    masterPwrs_[master] = {topPwr, botPwr};
  }
}

namespace edge_calc {
/**
 * @brief Calculates the difference between the passed parent_segment and the
 * vector segs The parent segment containts all the segments in the segs vector.
 * This function computes the difference between the parent segment and the
 * child segments. It first sorts the segs vector and merges intersecting ones.
 * Then it calculates the difference and returns a list of segments.
 */
std::vector<Rect> difference(const Rect& parent_segment,
                             const std::vector<Rect>& segs)
{
  if (segs.empty()) {
    return {parent_segment};
  }
  bool is_horizontal = parent_segment.yMin() == parent_segment.yMax();
  std::vector<Rect> sorted_segs = segs;
  // Sort segments by start coordinate
  std::sort(
      sorted_segs.begin(),
      sorted_segs.end(),
      [is_horizontal](const Rect& a, const Rect& b) {
        return (is_horizontal ? a.xMin() < b.xMin() : a.yMin() < b.yMin());
      });
  // Merge overlapping segments
  auto prev_seg = sorted_segs.begin();
  auto curr_seg = prev_seg;
  for (++curr_seg; curr_seg != sorted_segs.end();) {
    if (curr_seg->intersects(*prev_seg)) {
      prev_seg->merge(*curr_seg);
      curr_seg = sorted_segs.erase(curr_seg);
    } else {
      prev_seg = curr_seg++;
    }
  }
  // Get the difference
  const int start
      = is_horizontal ? parent_segment.xMin() : parent_segment.yMin();
  const int end = is_horizontal ? parent_segment.xMax() : parent_segment.yMax();
  int current_pos = start;
  std::vector<Rect> result;
  for (const Rect& seg : sorted_segs) {
    int seg_start = is_horizontal ? seg.xMin() : seg.yMin();
    int seg_end = is_horizontal ? seg.xMax() : seg.yMax();
    if (seg_start > current_pos) {
      if (is_horizontal) {
        result.emplace_back(current_pos,
                            parent_segment.yMin(),
                            seg_start,
                            parent_segment.yMax());
      } else {
        result.emplace_back(parent_segment.xMin(),
                            current_pos,
                            parent_segment.xMax(),
                            seg_start);
      }
    }
    current_pos = seg_end;
  }
  // Add the remaining end segment if it exists
  if (current_pos < end) {
    if (is_horizontal) {
      result.emplace_back(
          current_pos, parent_segment.yMin(), end, parent_segment.yMax());
    } else {
      result.emplace_back(
          parent_segment.xMin(), current_pos, parent_segment.xMax(), end);
    }
  }

  return result;
}

Rect getBoundarySegment(const Rect& bbox,
                        const odb::dbMasterEdgeType::EdgeDir dir)
{
  Rect segment(bbox);
  switch (dir) {
    case odb::dbMasterEdgeType::RIGHT:
      segment.set_xlo(bbox.xMax());
      break;
    case odb::dbMasterEdgeType::LEFT:
      segment.set_xhi(bbox.xMin());
      break;
    case odb::dbMasterEdgeType::TOP:
      segment.set_ylo(bbox.yMax());
      break;
    case odb::dbMasterEdgeType::BOTTOM:
      segment.set_yhi(bbox.yMin());
      break;
  }
  return segment;
}

}  // namespace edge_calc

Master* Optdp::getMaster(odb::dbMaster* db_master)
{
  auto min_row_height = std::numeric_limits<int>::max();
  for (dbRow* row : db_->getChip()->getBlock()->getRows()) {
    min_row_height = std::min(min_row_height, row->getSite()->getHeight());
  }

  const auto it = masterMap_.find(db_master);
  if (it != masterMap_.end()) {
    return it->second;
  }
  auto master = network_->createAndAddMaster();
  masterMap_[db_master] = master;
  Rect bbox;
  db_master->getPlacementBoundary(bbox);
  master->setBBox(bbox);
  master->clearEdges();
  if (!drc_engine_->hasCellEdgeSpacingTable()) {
    return master;
  }
  if (db_master->getType()
      == odb::dbMasterType::CORE_SPACER) {  // Skip fillcells
    return nullptr;
  }
  std::map<odb::dbMasterEdgeType::EdgeDir, std::vector<Rect>> typed_segs;
  int num_rows = std::lround(db_master->getHeight() / (double) min_row_height);
  for (auto edge : db_master->getEdgeTypes()) {
    auto dir = edge->getEdgeDir();
    Rect edge_rect = edge_calc::getBoundarySegment(bbox, dir);
    if (dir == odb::dbMasterEdgeType::TOP
        || dir == odb::dbMasterEdgeType::BOTTOM) {
      if (edge->getRangeBegin() != -1) {
        edge_rect.set_xlo(edge_rect.xMin() + edge->getRangeBegin());
        edge_rect.set_xhi(edge_rect.xMin() + edge->getRangeEnd());
      }
    } else {
      auto dy = edge_rect.dy();
      auto row_height = dy / num_rows;
      auto half_row_height = row_height / 2;
      if (edge->getCellRow() != -1) {
        edge_rect.set_ylo(edge_rect.yMin()
                          + (edge->getCellRow() - 1) * row_height);
        edge_rect.set_yhi(
            std::min(edge_rect.yMax(), edge_rect.yMin() + row_height));
      } else if (edge->getHalfRow() != -1) {
        edge_rect.set_ylo(edge_rect.yMin()
                          + (edge->getHalfRow() - 1) * half_row_height);
        edge_rect.set_yhi(
            std::min(edge_rect.yMax(), edge_rect.yMin() + half_row_height));
      }
    }
    typed_segs[dir].push_back(edge_rect);
    const auto edge_type_idx = drc_engine_->getEdgeTypeIdx(edge->getEdgeType());
    if (edge_type_idx != -1) {
      // consider only edge types defined in the spacing table
      master->addEdge(dpl::MasterEdge(edge_type_idx, edge_rect));
    }
  }
  const auto default_edge_type_idx = drc_engine_->getEdgeTypeIdx("DEFAULT");
  if (default_edge_type_idx == -1) {
    return master;
  }
  // Add the remaining DEFAULT un-typed segments
  for (size_t dir_idx = 0; dir_idx <= 3; dir_idx++) {
    const auto dir = (odb::dbMasterEdgeType::EdgeDir) dir_idx;
    const auto parent_seg = edge_calc::getBoundarySegment(bbox, dir);
    const auto default_segs
        = edge_calc::difference(parent_seg, typed_segs[dir]);
    for (const auto& seg : default_segs) {
      master->addEdge(dpl::MasterEdge(default_edge_type_idx, seg));
    }
  }
  return master;
}

////////////////////////////////////////////////////////////////
void Optdp::createNetwork()
{
  dbBlock* block = db_->getChip()->getBlock();
  auto core = block->getCoreArea();
  pwrLayers_.clear();
  gndLayers_.clear();

  // I allocate things statically, so I need to do some counting.

  auto block_insts = block->getInsts();
  std::vector<dbInst*> insts(block_insts.begin(), block_insts.end());
  std::stable_sort(insts.begin(), insts.end(), [](dbInst* a, dbInst* b) {
    return a->getName() < b->getName();
  });

  int nNodes = 0;
  for (dbInst* inst : insts) {
    // Skip instances which are not placeable.
    if (!inst->getMaster()->isCoreAutoPlaceable()) {
      continue;
    }
    ++nNodes;
  }

  dbSet<dbNet> nets = block->getNets();
  int nEdges = 0;
  int nPins = 0;
  for (dbNet* net : nets) {
    // Skip supply nets.
    if (net->getSigType().isSupply()) {
      continue;
    }
    ++nEdges;
    // Only count pins in insts if they considered
    // placeable since these are the only insts
    // that will be in our network.
    for (dbITerm* iTerm : net->getITerms()) {
      if (iTerm->getInst()->getMaster()->isCoreAutoPlaceable()) {
        ++nPins;
      }
    }
    // Count pins on terminals.
    nPins += net->getBTerms().size();
  }

  dbSet<dbBTerm> bterms = block->getBTerms();
  int nTerminals = 0;
  for (dbBTerm* bterm : bterms) {
    // Skip supply nets.
    dbNet* net = bterm->getNet();
    if (!net || net->getSigType().isSupply()) {
      continue;
    }
    ++nTerminals;
  }

  int nBlockages = 0;
  for (dbBlockage* blockage : block->getBlockages()) {
    if (!blockage->isSoft()) {
      auto box = blockage->getBBox()->getBox();
      box.moveDelta(-core.xMin(), -core.yMin());
      network_->createAndAddBlockage(box);
      ++nBlockages;
    }
  }

  logger_->info(DPO,
                100,
                "Creating network with {:d} cells, {:d} terminals, "
                "{:d} edges, {:d} pins, and {:d} blockages.",
                nNodes,
                nTerminals,
                nEdges,
                nPins,
                nBlockages);

  // Create and allocate the nodes.  I require nodes for
  // placeable instances as well as terminals.
  for (int i = 0; i < nNodes + nTerminals; i++) {
    network_->createAndAddNode();
  }
  for (int i = 0; i < nEdges; i++) {
    network_->createAndAddEdge();
  }

  // Return instances to a north orientation.  This makes
  // importing easier as I think it ensures all the pins,
  // etc. will be where I expect them to be.
  for (dbInst* inst : insts) {
    if (!inst->getMaster()->isCoreAutoPlaceable() || inst->isFixed()) {
      continue;
    }
    inst->setLocationOrient(dbOrientType::R0);  // Preserve lower-left.
  }

  // Populate nodes.
  int n = 0;
  for (dbInst* inst : insts) {
    if (!inst->getMaster()->isCoreAutoPlaceable()) {
      continue;
    }

    Node* ndi = network_->getNode(n);
    instMap_[inst] = ndi;

    // Name of inst.
    network_->setNodeName(n, inst->getName().c_str());

    // Fill in data.
    ndi->setType(Node::CELL);
    ndi->setDbInst(inst);
    ndi->setMaster(getMaster(inst->getMaster()));
    ndi->setId(n);
    ndi->setFixed(inst->isFixed());
    // else...  Account for R90?
    ndi->setOrient(odb::dbOrientType::R0);
    ndi->setHeight(DbuY{(int) inst->getMaster()->getHeight()});
    ndi->setWidth(DbuX{(int) inst->getMaster()->getWidth()});

    ndi->setOrigLeft(DbuX{inst->getBBox()->xMin() - core.xMin()});
    ndi->setOrigBottom(DbuY{inst->getBBox()->yMin() - core.yMin()});
    ndi->setLeft(ndi->getOrigLeft());
    ndi->setBottom(ndi->getOrigBottom());

    // Set the top and bottom power.
    auto it_m = masterPwrs_.find(inst->getMaster());
    if (masterPwrs_.end() == it_m) {
      ndi->setBottomPower(Architecture::Row::Power_UNK);
      ndi->setTopPower(Architecture::Row::Power_UNK);
    } else {
      ndi->setBottomPower(it_m->second.second);
      ndi->setTopPower(it_m->second.first);
    }

    ++n;  // Next node.
  }
  for (dbBTerm* bterm : bterms) {
    dbNet* net = bterm->getNet();
    if (!net || net->getSigType().isSupply()) {
      continue;
    }
    Node* ndi = network_->getNode(n);
    termMap_[bterm] = ndi;

    // Name of terminal.
    network_->setNodeName(n, bterm->getName().c_str());

    // Fill in data.
    ndi->setId(n);
    ndi->setType(Node::TERMINAL);
    ndi->setFixed(true);
    ndi->setOrient(odb::dbOrientType::R0);

    DbuX ww(bterm->getBBox().xMax() - bterm->getBBox().xMin());
    DbuY hh(bterm->getBBox().yMax() - bterm->getBBox().yMax());

    ndi->setHeight(hh);
    ndi->setWidth(ww);

    ndi->setOrigLeft(DbuX{bterm->getBBox().xMin() - core.xMin()});
    ndi->setOrigBottom(DbuY{bterm->getBBox().yMin() - core.yMin()});
    ndi->setLeft(ndi->getOrigLeft());
    ndi->setBottom(ndi->getOrigBottom());

    // Not relevant for terminal.
    ndi->setBottomPower(Architecture::Row::Power_UNK);
    ndi->setTopPower(Architecture::Row::Power_UNK);

    ++n;  // Next node.
  }
  if (n != nNodes + nTerminals) {
    logger_->error(DPO,
                   101,
                   "Unexpected total node count.  Expected {:d}, but got {:d}",
                   (nNodes + nTerminals),
                   n);
  }

  // Populate edges and pins.
  int e = 0;
  int p = 0;
  for (dbNet* net : nets) {
    // Skip supply nets.
    if (net->getSigType().isSupply()) {
      continue;
    }

    Edge* edi = network_->getEdge(e);
    edi->setId(e);
    netMap_[net] = edi;

    // Name of edge.
    network_->setEdgeName(e, net->getName().c_str());

    for (dbITerm* iTerm : net->getITerms()) {
      if (!iTerm->getInst()->getMaster()->isCoreAutoPlaceable()) {
        continue;
      }

      auto it_n = instMap_.find(iTerm->getInst());
      if (instMap_.end() != it_n) {
        n = it_n->second->getId();  // The node id.

        if (network_->getNode(n)->getId() != n
            || network_->getEdge(e)->getId() != e) {
          logger_->error(
              DPO, 102, "Improper node indexing while connecting pins.");
        }

        Pin* ptr = network_->createAndAddPin(network_->getNode(n),
                                             network_->getEdge(e));

        // Pin offset.
        dbMTerm* mTerm = iTerm->getMTerm();
        dbMaster* master = mTerm->getMaster();
        // Due to old bookshelf, my offsets are from the
        // center of the cell whereas in DEF, it's from
        // the bottom corner.
        auto ww = mTerm->getBBox().dx();
        auto hh = mTerm->getBBox().dy();
        auto xx = mTerm->getBBox().xCenter();
        auto yy = mTerm->getBBox().yCenter();
        auto dx = xx - ((int) master->getWidth() / 2);
        auto dy = yy - ((int) master->getHeight() / 2);

        ptr->setOffsetX(DbuX{dx});
        ptr->setOffsetY(DbuY{dy});
        ptr->setPinHeight(DbuY{hh});
        ptr->setPinWidth(DbuX{ww});
        ptr->setPinLayer(0);  // Set to zero since not currently used.

        ++p;  // next pin.
      } else {
        logger_->error(
            DPO,
            103,
            "Could not find node for instance while connecting pins.");
      }
    }
    for (dbBTerm* bTerm : net->getBTerms()) {
      auto it_p = termMap_.find(bTerm);
      if (termMap_.end() != it_p) {
        n = it_p->second->getId();  // The node id.

        if (network_->getNode(n)->getId() != n
            || network_->getEdge(e)->getId() != e) {
          logger_->error(
              DPO, 104, "Improper terminal indexing while connecting pins.");
        }

        Pin* ptr = network_->createAndAddPin(network_->getNode(n),
                                             network_->getEdge(e));

        // These don't need an offset.
        ptr->setOffsetX(DbuX{0});
        ptr->setOffsetY(DbuY{0});
        ptr->setPinHeight(DbuY{0});
        ptr->setPinWidth(DbuX{0});
        ptr->setPinLayer(0);  // Set to zero since not currently used.

        ++p;  // next pin.
      } else {
        logger_->error(
            DPO,
            105,
            "Could not find node for terminal while connecting pins.");
      }
    }

    ++e;  // next edge.
  }
  if (e != nEdges) {
    logger_->error(DPO,
                   106,
                   "Unexpected total edge count.  Expected {:d}, but got {:d}",
                   nEdges,
                   e);
  }
  if (p != nPins) {
    logger_->error(DPO,
                   107,
                   "Unexpected total pin count.  Expected {:d}, but got {:d}",
                   nPins,
                   p);
  }

  logger_->info(DPO,
                109,
                "Network stats: inst {}, edges {}, pins {}",
                network_->getNumNodes(),
                network_->getNumEdges(),
                network_->getNumPins());
}
////////////////////////////////////////////////////////////////
void Optdp::createArchitecture()
{
  dbBlock* block = db_->getChip()->getBlock();
  auto core = block->getCoreArea();

  auto min_row_height = std::numeric_limits<int>::max();
  for (dbRow* row : block->getRows()) {
    min_row_height = std::min(min_row_height, row->getSite()->getHeight());
  }

  std::map<int, std::unordered_set<std::string>> skip_list;

  for (dbRow* row : block->getRows()) {
    if (row->getSite()->getClass() == odb::dbSiteClass::PAD) {
      continue;
    }
    if (row->getDirection() != odb::dbRowDir::HORIZONTAL) {
      // error.
      continue;
    }
    dbSite* site = row->getSite();
    if (site->getHeight() > min_row_height) {
      skip_list[site->getHeight()].insert(site->getName());
      continue;
    }
    odb::Point origin = row->getOrigin();

    Architecture::Row* archRow = arch_->createAndAddRow();

    archRow->setSubRowOrigin(origin.x() - core.xMin());
    archRow->setBottom(origin.y() - core.yMin());
    archRow->setSiteSpacing(row->getSpacing());
    archRow->setNumSites(row->getSiteCount());
    archRow->setSiteWidth(site->getWidth());
    archRow->setHeight(site->getHeight());

    // Set defaults.  Top and bottom power is set below.
    archRow->setBottomPower(Architecture::Row::Power_UNK);
    archRow->setTopPower(Architecture::Row::Power_UNK);

    // Symmetry.  From the site.
    unsigned symmetry = 0x00000000;
    if (site->getSymmetryX()) {
      symmetry |= dpo::Symmetry_X;
    }
    if (site->getSymmetryY()) {
      symmetry |= dpo::Symmetry_Y;
    }
    if (site->getSymmetryR90()) {
      symmetry |= dpo::Symmetry_ROT90;
    }
    archRow->setSymmetry(symmetry);

    // Orientation.  From the row.
    archRow->setOrient(row->getOrient());
  }
  for (const auto& skip : skip_list) {
    std::string skip_string = "[";
    int i = 0;
    for (const auto& skipped_site : skip.second) {
      skip_string += skipped_site + ",]"[i == skip.second.size() - 1];
      ++i;
    }
    logger_->warn(DPO,
                  108,
                  "Skipping all the rows with sites {} as their height is {} "
                  "and the single-height is {}.",
                  skip_string,
                  skip.first,
                  min_row_height);
  }
  // Get surrounding box.
  {
    int xmin = std::numeric_limits<int>::max();
    int xmax = std::numeric_limits<int>::lowest();
    int ymin = std::numeric_limits<int>::max();
    int ymax = std::numeric_limits<int>::lowest();
    for (int r = 0; r < arch_->getNumRows(); r++) {
      Architecture::Row* row = arch_->getRow(r);

      xmin = std::min(xmin, row->getLeft());
      xmax = std::max(xmax, row->getRight());
      ymin = std::min(ymin, row->getBottom());
      ymax = std::max(ymax, row->getTop());
    }
    arch_->setMinX(xmin);
    arch_->setMaxX(xmax);
    arch_->setMinY(ymin);
    arch_->setMaxY(ymax);
  }

  for (int r = 0; r < arch_->getNumRows(); r++) {
    int numSites = arch_->getRow(r)->getNumSites();
    int originX = arch_->getRow(r)->getLeft();
    int siteSpacing = arch_->getRow(r)->getSiteSpacing();
    int siteWidth = arch_->getRow(r)->getSiteWidth();
    const int endGap = siteWidth - siteSpacing;
    if (originX < arch_->getMinX()) {
      originX = arch_->getMinX();
      if (arch_->getRow(r)->getLeft() != originX) {
        arch_->getRow(r)->setSubRowOrigin(originX);
      }
    }
    if (originX + numSites * siteSpacing + endGap > arch_->getMaxX()) {
      numSites = (arch_->getMaxX() - endGap - originX) / siteSpacing;
      if (arch_->getRow(r)->getNumSites() != numSites) {
        arch_->getRow(r)->setNumSites(numSites);
      }
    }
  }

  // Need the power running across the bottom and top of each
  // row.  I think the way to do this is to look for power
  // and ground nets and then look at the special wires.
  // Not sure, though, of the best way to pick those that
  // actually touch the cells (i.e., which layer?).
  for (dbNet* net : block->getNets()) {
    if (!net->isSpecial()) {
      continue;
    }
    if (!(net->getSigType() == dbSigType::POWER
          || net->getSigType() == dbSigType::GROUND)) {
      continue;
    }
    int pwr = (net->getSigType() == dbSigType::POWER)
                  ? Architecture::Row::Power_VDD
                  : Architecture::Row::Power_VSS;
    for (dbSWire* swire : net->getSWires()) {
      if (swire->getWireType() != dbWireType::ROUTED) {
        continue;
      }

      for (dbSBox* sbox : swire->getWires()) {
        if (sbox->getDirection() != dbSBox::HORIZONTAL) {
          continue;
        }
        if (sbox->isVia()) {
          continue;
        }
        dbTechLayer* layer = sbox->getTechLayer();
        if (pwr == Architecture::Row::Power_VDD) {
          if (pwrLayers_.end() == pwrLayers_.find(layer)) {
            continue;
          }
        } else if (pwr == Architecture::Row::Power_VSS) {
          if (gndLayers_.end() == gndLayers_.find(layer)) {
            continue;
          }
        }

        Rect rect = sbox->getBox();
        rect.moveDelta(core.xMin(), core.yMin());
        for (size_t r = 0; r < arch_->getNumRows(); r++) {
          int yb = arch_->getRow(r)->getBottom();
          int yt = arch_->getRow(r)->getTop();

          if (yb >= rect.yMin() && yb <= rect.yMax()) {
            arch_->getRow(r)->setBottomPower(pwr);
          }
          if (yt >= rect.yMin() && yt <= rect.yMax()) {
            arch_->getRow(r)->setTopPower(pwr);
          }
        }
      }
    }
  }
  arch_->postProcess(network_);
}
////////////////////////////////////////////////////////////////
void Optdp::createGrid()
{
  grid_->init(logger_);
  grid_->initBlock(db_->getChip()->getBlock());
  grid_->clear();
  grid_->examineRows(db_->getChip()->getBlock());
  grid_->initGrid(
      db_, db_->getChip()->getBlock(), std::make_shared<dpl::Padding>(), 0, 0);
}
////////////////////////////////////////////////////////////////
void Optdp::setUpPlacementGroups()
{
  int xmin = arch_->getMinX();
  int xmax = arch_->getMaxX();
  int ymin = arch_->getMinY();
  int ymax = arch_->getMaxY();

  dbBlock* block = db_->getChip()->getBlock();
  auto core = block->getCoreArea();
  std::unordered_map<odb::dbInst*, Node*>::iterator it_n;
  Group* rptr = nullptr;
  int count = 0;

  // Default region.
  rptr = arch_->createAndAddRegion();
  rptr->setId(count++);

  rptr->addRect({xmin, ymin, xmax, ymax});
  rptr->setBoundary(Rect(xmin, ymin, xmax, ymax));
  // rptr->setMinX(xmin);
  // rptr->setMaxX(xmax);
  // rptr->setMinY(ymin);
  // rptr->setMaxY(ymax);

  auto db_groups = block->getGroups();
  for (auto db_group : db_groups) {
    dbRegion* parent = db_group->getRegion();
    if (parent) {
      rptr = arch_->createAndAddRegion();
      rptr->setId(count);
      ++count;
      auto boundaries = parent->getBoundaries();
      Rect bbox;
      bbox.mergeInit();
      for (dbBox* boundary : boundaries) {
        Rect box = boundary->getBox();
        box.moveDelta(-core.xMin(), -core.yMin());
        xmin = std::max(arch_->getMinX(), box.xMin());
        xmax = std::min(arch_->getMaxX(), box.xMax());
        ymin = std::max(arch_->getMinY(), box.yMin());
        ymax = std::min(arch_->getMaxY(), box.yMax());

        rptr->addRect({xmin, ymin, xmax, ymax});
        bbox.merge({xmin, ymin, xmax, ymax});
      }
      rptr->setBoundary(bbox);

      // The instances within this region.
      for (auto db_inst : db_group->getInsts()) {
        it_n = instMap_.find(db_inst);
        if (instMap_.end() != it_n) {
          Node* nd = it_n->second;
          if (nd->getGroupId() == 0) {
            nd->setGroupId(rptr->getId());
            nd->setGroup(rptr);
          }
        }
      }
    }
  }
  logger_->info(DPO, 110, "Number of regions is {:d}", arch_->getNumRegions());
}
}  // namespace dpo

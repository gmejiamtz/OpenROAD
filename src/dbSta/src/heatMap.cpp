// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "heatMap.h"

#include <string>
#include <vector>

#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "sta/Corner.hh"

namespace sta {

PowerDensityDataSource::PowerDensityDataSource(sta::dbSta* sta,
                                               utl::Logger* logger)
    : gui::RealValueHeatMapDataSource(logger,
                                      "W",
                                      "Power Density",
                                      "Power",
                                      "PowerDensity"),
      sta_(sta)
{
  setIssueRedraw(false);  // disable during initial setup
  setLogScale(true);
  setIssueRedraw(true);

  addMultipleChoiceSetting(
      "Corner",
      "Corner:",
      [this]() {
        std::vector<std::string> corners;
        for (auto* corner : *sta_->corners()) {
          corners.emplace_back(corner->name());
        }
        return corners;
      },
      [this]() -> std::string { return corner_; },
      [this](const std::string& value) { corner_ = value; });
  addBooleanSetting(
      "Internal",
      "Internal power:",
      [this]() { return include_internal_; },
      [this](bool value) { include_internal_ = value; });
  addBooleanSetting(
      "Leakage",
      "Leakage power:",
      [this]() { return include_leakage_; },
      [this](bool value) { include_leakage_ = value; });
  addBooleanSetting(
      "Switching",
      "Switching power:",
      [this]() { return include_switching_; },
      [this](bool value) { include_switching_ = value; });

  registerHeatMap();
}

bool PowerDensityDataSource::populateMap()
{
  if (getBlock() == nullptr || sta_ == nullptr) {
    return false;
  }

  if (sta_->cmdNetwork() == nullptr) {
    return false;
  }

  auto* network = sta_->getDbNetwork();

  const bool include_all
      = include_internal_ && include_leakage_ && include_switching_;
  for (auto* inst : getBlock()->getInsts()) {
    if (!inst->getPlacementStatus().isPlaced()) {
      continue;
    }

    sta::PowerResult power = sta_->power(network->dbToSta(inst), getCorner());

    float pwr = 0.0;
    if (include_all) {
      pwr = power.total();
    } else {
      if (include_internal_) {
        pwr += power.internal();
      }
      if (include_leakage_) {
        pwr += power.switching();
      }
      if (include_switching_) {
        pwr += power.leakage();
      }
    }

    odb::Rect inst_box = inst->getBBox()->getBox();

    addToMap(inst_box, pwr);
  }

  return true;
}

void PowerDensityDataSource::combineMapData(bool base_has_value,
                                            double& base,
                                            const double new_data,
                                            const double data_area,
                                            const double intersection_area,
                                            const double rect_area)
{
  base += (new_data / data_area) * intersection_area;
}

sta::Corner* PowerDensityDataSource::getCorner() const
{
  auto* corner = sta_->findCorner(corner_.c_str());
  if (corner != nullptr) {
    return corner;
  }

  auto corners = sta_->corners()->corners();
  if (!corners.empty()) {
    return corners[0];
  }

  return nullptr;
}

}  // namespace sta

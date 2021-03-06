/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmPortGroup.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmPortTable.h"
#include "fboss/agent/hw/bcm/BcmPort.h"
#include "fboss/agent/hw/bcm/BcmError.h"

#include "fboss/agent/state/Port.h"

extern "C" {
#include <opennsl/port.h>
}

namespace {
using facebook::fboss::BcmPortGroup;
using facebook::fboss::cfg::PortSpeed;
using facebook::fboss::FbossError;
BcmPortGroup::LaneMode neededLaneModeForSpeed(
    PortSpeed speed, PortSpeed maxLaneSpeed) {
  if (speed == PortSpeed::DEFAULT) {
    throw FbossError("Speed cannot be DEFAULT");
  }
  auto neededLanes =
    static_cast<uint32_t>(speed) / static_cast<uint32_t>(maxLaneSpeed);
  if (neededLanes <= 1) {
    return BcmPortGroup::LaneMode::QUAD;
  } else if (neededLanes == 2) {
    return BcmPortGroup::LaneMode::DUAL;
  } else if (neededLanes > 2 && neededLanes <= 4) {
    return BcmPortGroup::LaneMode::SINGLE;
  } else {
    throw FbossError("Cannot support speed ", speed);
  }
}

}

namespace facebook { namespace fboss {

BcmPortGroup::BcmPortGroup(BcmSwitch* hw,
                           BcmPort* controllingPort,
                           std::vector<BcmPort*> allPorts)
    : hw_(hw),
      controllingPort_(controllingPort),
      allPorts_(std::move(allPorts)) {

  if (allPorts_.size() != 4) {
    throw FbossError("Port groups must have exactly four members");
  }

  for (int i = 0; i < allPorts_.size(); ++i) {
    if (getLane(allPorts_[i]) != i) {
      throw FbossError("Ports passed in are not ordered by lane");
    }
  }

  // get the number of active lanes
  auto activeLanes = retrieveActiveLanes();
  switch (activeLanes) {
    case 1:
      laneMode_ = LaneMode::QUAD;
      break;
    case 2:
      laneMode_ = LaneMode::DUAL;
      break;
    case 4:
      laneMode_ = LaneMode::SINGLE;
      break;
    default:
      throw FbossError("Unexpected number of lanes retrieved for bcm port ",
                       controllingPort_->getBcmPortId());
  }

}

BcmPortGroup::~BcmPortGroup() {}

BcmPortGroup::LaneMode BcmPortGroup::calculateDesiredLaneMode(
    const std::vector<Port*>& ports, cfg::PortSpeed maxLaneSpeed) {
  auto desiredMode = LaneMode::QUAD;
  for (int lane = 0; lane < ports.size(); ++lane) {
    auto port = ports[lane];
    if (!port->isDisabled()) {
      auto neededMode = neededLaneModeForSpeed(port->getSpeed(), maxLaneSpeed);
      if (neededMode < desiredMode) {
        desiredMode = neededMode;
      }

      // Check that the lane is expected for SINGLE/DUAL modes
      if (desiredMode == LaneMode::SINGLE) {
        if (lane != 0) {
          throw FbossError("Only lane 0 can be enabled in SINGLE mode");
        }
      } else if (desiredMode == LaneMode::DUAL) {
        if (lane != 0 && lane != 2) {
          throw FbossError("Only lanes 0 or 2 can be enabled in DUAL mode");
        }
      }

      VLOG(3) << "Port " << port->getID() << " enabled with speed " <<
        static_cast<int>(port->getSpeed());
    }
  }
  return desiredMode;
}

BcmPortGroup::LaneMode BcmPortGroup::getDesiredLaneMode(
    const std::shared_ptr<SwitchState>& state) const {
  std::vector<Port*> ports;
  for (auto bcmPort : allPorts_) {
    auto swPort = bcmPort->getSwitchStatePort(state).get();
    // Make sure the ports support the configured speed.
    // We check this even if the port is disabled.
    if (!bcmPort->supportsSpeed(swPort->getSpeed())) {
      throw FbossError("Port ", swPort->getID(), " does not support speed ",
                       static_cast<int>(swPort->getSpeed()));
    }
    ports.push_back(swPort);
  }
  return calculateDesiredLaneMode(ports, controllingPort_->maxLaneSpeed());
}

uint8_t BcmPortGroup::getLane(const BcmPort* bcmPort) const {
  return bcmPort->getBcmPortId() - controllingPort_->getBcmPortId();
}

bool BcmPortGroup::validConfiguration(
    const std::shared_ptr<SwitchState>& state) const {
  try {
    getDesiredLaneMode(state);
  } catch (const std::exception& ex) {
    VLOG(1) << "Received exception determining lane mode: " << ex.what();
    return false;
  }
  return true;
}

void BcmPortGroup::reconfigureIfNeeded(
  const std::shared_ptr<SwitchState>& state) {
  // This logic is a bit messy. We could encode some notion of port
  // groups into the swith state somehow so it is easy to generate
  // deltas for these. For now, we need pass around the SwitchState
  // object and get the relevant ports manually.
  auto desiredLaneMode = getDesiredLaneMode(state);

  if (desiredLaneMode != laneMode_) {
    reconfigure(state, desiredLaneMode);
  }
}

void BcmPortGroup::reconfigure(
  const std::shared_ptr<SwitchState>& state,
  LaneMode newLaneMode
) {
  // The logic for this follows the steps required for flex-port support
  // outlined in the sdk documentation.
  VLOG(1) << "Reconfiguring port " << controllingPort_->getBcmPortId()
          << " from " << laneMode_ << " active ports to " << newLaneMode
          << " active ports";

  // 1. disable all group members
  for (auto& bcmPort : allPorts_) {
    auto swPort = bcmPort->getSwitchStatePort(state);
    bcmPort->disable(swPort);
  }

  // 2. remove all ports from the counter DMA and linkscan bitmaps
  // This is done in BcmPort::disable()

  // 3. set the opennslPortControlLanes setting
  setActiveLanes(newLaneMode);

  // 4. enable ports
  for (auto& bcmPort : allPorts_) {
    auto swPort = bcmPort->getSwitchStatePort(state);
    if (!swPort->isDisabled()) {
      bcmPort->enable(swPort);
    }
  }

  // 5. add ports to the counter DMA + linkscan
  // This is done in BcmPort::enable()
}

}} // namespace facebook::fboss

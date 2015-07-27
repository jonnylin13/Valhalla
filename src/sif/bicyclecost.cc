#include "sif/bicyclecost.h"
#include <valhalla/baldr/directededge.h>
#include <valhalla/baldr/nodeinfo.h>
#include <valhalla/midgard/constants.h>

using namespace valhalla::baldr;

namespace valhalla {
namespace sif {

// Default options/values
namespace {
constexpr float kDefaultManeuverPenalty         = 5.0f;   // Seconds
constexpr float kDefaultDestinationOnlyPenalty  = 60.0f;  // Seconds
constexpr float kDefaultAlleyPenalty            = 60.0f;   // Seconds
constexpr float kDefaultGateCost                = 30.0f;  // Seconds
constexpr float kDefaultCountryCrossingCost     = 600.0f; // Seconds
constexpr float kDefaultCountryCrossingPenalty  = 0.0f;   // Seconds

// Default turn costs
constexpr float kTCStraight         = 0.5f;
constexpr float kTCSlight           = 0.75f;
constexpr float kTCFavorable        = 1.0f;
constexpr float kTCFavorableSharp   = 1.5f;
constexpr float kTCCrossing         = 2.0f;
constexpr float kTCUnfavorable      = 2.5f;
constexpr float kTCUnfavorableSharp = 3.5f;
constexpr float kTCReverse          = 5.0f;

// Cost of traversing an edge with steps. Make this high but not impassible.
// Equal to about 0.5km.
const Cost kBicycleStepsCost = { 500.0f, 30.0f };

// How much to favor bicycle networks. TODO - make a config option
constexpr float kBicycleNetworkFactor = 0.8f;

constexpr float kDefaultCyclingSpeed   = 25.0f;  // ~15.5 MPH
constexpr float kDefaultUseRoadsFactor = 0.5f;   // Default factor for roads
constexpr float kDefaultSmoothFactor   = 0.9f;   // Prefer smooth surfaces
constexpr float kDefaultSurfaceFactor  = 0.2f;   // TODO
}

/**
 * Derived class providing dynamic edge costing for bicycle routes.
 */
class BicycleCost : public DynamicCost {
 public:
  /**
   * Constructor. Configuration / options for bicycle costing are provided
   * via a property tree.
   * @param  config  Property tree with configuration/options.
   */
  BicycleCost(const boost::property_tree::ptree& config);

  virtual ~BicycleCost();

  /**
   * Checks if access is allowed for the provided directed edge.
   * This is generally based on mode of travel and the access modes
   * allowed on the edge. However, it can be extended to exclude access
   * based on other parameters.
   * @param  edge  Pointer to a directed edge.
   * @param  pred  Predecessor edge information.
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::DirectedEdge* edge,
                       const EdgeLabel& pred) const;

  /**
   * Checks if access is allowed for an edge on the reverse path
   * (from destination towards origin). Both opposing edges are
   * provided.
   * @param  edge  Pointer to a directed edge.
   * @param  opp_edge  Pointer to the opposing directed edge.
   * @param  opp_pred_edge  Pointer to the opposing directed edge to the
   *                        predecessor.
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool AllowedReverse(const baldr::DirectedEdge* edge,
                 const baldr::DirectedEdge* opp_edge,
                 const baldr::DirectedEdge* opp_pred_edge) const;

  /**
   * Checks if access is allowed for the provided node. Node access can
   * be restricted if bollards or gates are present. (TODO - others?)
   * @param  edge  Pointer to node information.
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::NodeInfo* node) const;

  /**
   * Get the cost to traverse the specified directed edge. Cost includes
   * the time (seconds) to traverse the edge.
   * @param   edge  Pointer to a directed edge.
   * @param   density  Relative road density.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge* edge,
                        const uint32_t density) const;

  /**
   * Returns the cost to make the transition from the predecessor edge.
   * Defaults to 0. Costing models that wish to include edge transition
   * costs (i.e., intersection/turn costs) must override this method.
   * @param  edge  Directed edge (the to edge)
   * @param  node  Node (intersection) where transition occurs.
   * @param  pred  Predecessor edge information.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCost(const baldr::DirectedEdge* edge,
                              const baldr::NodeInfo* node,
                              const EdgeLabel& pred) const;

  /**
   * Get the cost factor for A* heuristics. This factor is multiplied
   * with the distance to the destination to produce an estimate of the
   * minimum cost to the destination. The A* heuristic must underestimate the
   * cost to the destination. So a time based estimate based on speed should
   * assume the maximum speed is used to the destination such that the time
   * estimate is less than the least possible time along roads.
   */
  virtual float AStarCostFactor() const;

 protected:
  enum class BicycleType {
    kRoad     = 0,
    kCross    = 1,
    kHybrid   = 2,
    kMountain = 3
  };

  float speedfactor_[100];          // Cost factors based on speed in kph
  float density_factor_[16];        // Density factor
  float maneuver_penalty_;          // Penalty (seconds) when inconsistent names
  float destination_only_penalty_;  // Penalty (seconds) using a driveway or parking aisle
  float gate_cost_;                 // Cost (seconds) to go through gate
  float alley_penalty_;             // Penalty (seconds) to use a alley
  float country_crossing_cost_;     // Cost (seconds) to go through toll booth
  float country_crossing_penalty_;  // Penalty (seconds) to go across a country border

  // Density factor used in edge transition costing
  std::vector<float> trans_density_factor_;

  // Bicycling speed (default to 25 kph)
  float speed_;

  // Bicycle type
  BicycleType bicycletype_;

  // Weighting applied for the smoothest surfaces. Weighting for other
  // surfaces starts at this factor and adds the bicycle surface factor
  // times the surface smoothness.
  float smooth_surface_factor_;

  // Bicycle surface factor. An indication of how much the bicycle type
  // influences selection of surface types. Road bikes may be more
  // influenced by surface type than other bikes. Negative values are
  // allowed if one wants to favor rougher surfaces, but keep the value to
  // no more than -0.1
  // TODO - elaborate more on how the weighting occurs

  float bicycle_surface_factor_;

  // A measure of willingness to ride with traffic. Ranges from 0-1 with
  // 0 being not willing at all and 1 being totally comfortable. This factor
  // determines how much cycle lanes and paths are preferred over roads (if
  // at all). Experienced road riders and messengers may use a value = 1
  // while beginners may use a value of 0.1 to stay away from roads unless
  // absolutely necessary.
  float useroads_;

  /**
   * Compute a turn cost based on the turn type, crossing flag,
   * and whether right or left side of road driving.
   * @param  turn_type  Turn type (see baldr/turn.h)
   * @param  crossing   Crossing another road if true.
   * @param  drive_on_right  Right hand side of road driving if true.
   */
  float TurnCost(const baldr::Turn::Type turn_type, const bool crossing,
                 const bool drive_on_right) const;

 public:
  /**
   * Returns a function/functor to be used in location searching which will
   * exclude results from the search by looking at each edges attribution
   * @return Function to be used in filtering out edges
   */
  virtual const EdgeFilter GetFilter() const {
    //throw back a lambda that checks the access for this type of costing
    BicycleType b = bicycletype_;
    return [b](const baldr::DirectedEdge* edge){
      // Prohibit certain roads based on surface type and bicycle type
      if(!edge->trans_up() && !edge->trans_down() &&
        (edge->forwardaccess() & kBicycleAccess)) {
        if (b == BicycleType::kRoad)
          return edge->surface() > Surface::kPavedRough;
        else if (b == BicycleType::kHybrid)
          return edge->surface() > Surface::kCompacted;
        else if (b == BicycleType::kCross)
          return edge->surface() > Surface::kDirt;
        else if (b == BicycleType::kMountain)
          return edge->surface() >= Surface::kPath;
      }
      return true;
    };
  }
};

// Bicycle route costs are distance based with some favor/avoid based on
// attribution.
// TODO - add options and config settings.
// TODO - how to handle time/speed for estimating time on path

// Constructor
BicycleCost::BicycleCost(const boost::property_tree::ptree& pt)
    : DynamicCost(pt, TravelMode::kBicycle),
      trans_density_factor_{ 1.0f, 1.0f, 1.0f, 1.0f,
                             1.0f, 1.1f, 1.2f, 1.3f,
                             1.4f, 1.6f, 1.9f, 2.2f,
                             2.5f, 2.8f, 3.1f, 3.5f } {

  maneuver_penalty_ = pt.get<float>("maneuver_penalty",
                                    kDefaultManeuverPenalty);
  destination_only_penalty_ = pt.get<float>("destination_only_penalty",
                                            kDefaultDestinationOnlyPenalty);
  gate_cost_ = pt.get<float>("gate_cost", kDefaultGateCost);
  alley_penalty_ = pt.get<float>("alley_penalty",
                                 kDefaultAlleyPenalty);
  country_crossing_cost_ = pt.get<float>("country_crossing_cost",
                                           kDefaultCountryCrossingCost);
  country_crossing_penalty_ = pt.get<float>("country_crossing_penalty",
                                           kDefaultCountryCrossingPenalty);

  smooth_surface_factor_  = pt.get<float>("smoothness_factor", kDefaultSmoothFactor);
  bicycle_surface_factor_ = pt.get<float>("surface_factor", kDefaultSurfaceFactor);
  useroads_    = pt.get<float>("useroads_", kDefaultUseRoadsFactor);
  speed_       = pt.get<float>("cycling_speed", kDefaultCyclingSpeed);

  // TODO - bicycle type - enter as string and convert to enum
  bicycletype_ = BicycleType::kRoad;

  // Create speed cost table (to avoid division in costing)
  speedfactor_[0] = kSecPerHour;
  for (uint32_t s = 1; s < 100; s++) {
    speedfactor_[s] = (kSecPerHour * 0.001f) / static_cast<float>(s);
  }
}

// Destructor
BicycleCost::~BicycleCost() {
}

// Check if access is allowed on the specified edge.
bool BicycleCost::Allowed(const baldr::DirectedEdge* edge,
                          const EdgeLabel& pred) const {
  // Check bicycle access and turn restrictions. Bicycles should obey
  // vehicular turn restrictions. Disallow Uturns. Do not allow entering
  // not-thru edges except near the destination. Skip impassable edges.
  if (!(edge->forwardaccess() & kBicycleAccess) ||
      (pred.opp_local_idx() == edge->localedgeidx()) ||
      (pred.restrictions() & (1 << edge->localedgeidx())) ||
      (edge->not_thru() && pred.distance() > not_thru_distance_)) {
    return false;
  }

  // Prohibit certain roads based on surface type and bicycle type
  if (bicycletype_ == BicycleType::kRoad)
    return edge->surface() <= Surface::kPavedRough;
  else if (bicycletype_ == BicycleType::kHybrid)
      return edge->surface() <= Surface::kCompacted;
  else if (bicycletype_ == BicycleType::kCross)
    return edge->surface() <= Surface::kDirt;
  else // if (bicycletype_ == BicycleType::kMountain)
    return edge->surface() <= Surface::kPath;   // Allow all but impassable
}

// Checks if access is allowed for an edge on the reverse path (from
// destination towards origin). Both opposing edges are provided.
bool BicycleCost::AllowedReverse(const baldr::DirectedEdge* edge,
               const baldr::DirectedEdge* opp_edge,
               const baldr::DirectedEdge* opp_pred_edge) const {
  // Check access, U-turn, and simple turn restriction.
  // Check if edge is not-thru (no need to check distance from destination
  // since the search is heading out of any not_thru regions)
  if (!(opp_edge->forwardaccess() & kBicycleAccess) ||
       (opp_pred_edge->localedgeidx() == edge->localedgeidx()) ||
       (opp_edge->restrictions() & (1 << opp_pred_edge->localedgeidx())) ||
        edge->not_thru()) {
    return false;
  }

  // Prohibit certain roads based on surface type and bicycle type
  if (bicycletype_ == BicycleType::kRoad)
    return opp_edge->surface() <= Surface::kPavedRough;
  else if (bicycletype_ == BicycleType::kHybrid)
      return opp_edge->surface() <= Surface::kCompacted;
  else if (bicycletype_ == BicycleType::kCross)
    return opp_edge->surface() <= Surface::kDirt;
  else // if (bicycletype_ == BicycleType::kMountain)
    return opp_edge->surface() <= Surface::kPath; // Allow all but impassable
}

// Check if access is allowed at the specified node.
bool BicycleCost::Allowed(const baldr::NodeInfo* node) const {
  return (node->access() & kBicycleAccess);
}

// Returns the cost to traverse the edge and an estimate of the actual time
// (in seconds) to traverse the edge.
Cost BicycleCost::EdgeCost(const baldr::DirectedEdge* edge,
                           const uint32_t density) const {
  // Stairs/steps - use a high fixed cost so they are generally avoided.
   if (edge->use() == Use::kSteps) {
     return kBicycleStepsCost;
   }

   // Alter speed and apply a weighting factor to the cost based on
   // desirability of cycling on this edge. Based on several factors:
   // type of bike, rider propensity to ride on roads, surface, use type
   // of road, presence of bike lanes, belonging to a bike network, and
   // the hilliness/elevation change.
   float factor = 1.0f;
   float speed  = speed_;

   // Surface factor - TODO - alter speed
   float surface_factor = smooth_surface_factor_ + (bicycle_surface_factor_ *
           static_cast<uint32_t>(edge->surface()));
   factor *= surface_factor;

   // Favor dedicated cycleways (favor more if rider for riders not wanting
   // to use roadways! Footways with cycle access - favor unless the rider
   // wants to favor use of roads
   if (edge->use() == Use::kCycleway) {
     factor *= 0.65f;
   } else if (edge->use() == Use::kFootway) {
     // Cyclists who favor using roads want to avoid footways. A value of
     // 0.5 for useroads_ will result in no difference using footway
     factor *= 0.75f + (useroads_ * 0.5f);
   } else {
     if (edge->cyclelane() == CycleLane::kNone) {
       // No cycle lane exists, base the factor on auto speed on the road.
       // Roads < 65 kph are neutral. Start avoiding based on speed above this.
       // TODO - may want to avoid very low speed edges, alleys/service?
       float speedfactor = (edge->speed() < 65.0f) ? 1.0f :
             (1.0f + (edge->speed() - 65.0f) * (1.0f - useroads_) * 0.05f);
       factor *= speedfactor;
     } else {
       // A cycle lane exists. Favor separated lanes more than dedicated
       // and shared use.
       float laneusefactor = 0.75f + useroads_ * 0.05f;
       factor *= (laneusefactor + static_cast<uint32_t>(edge->cyclelane()) * 0.1f);
     }
   }

   // Slightly favor bicycle networks.
   // TODO - do we need to differentiate between types of network?
   if (edge->bikenetwork() > 0) {
     factor *= kBicycleNetworkFactor;
   }

   // Compute elapsed time and cost (with weight factors)
   float sec = (edge->length() * speedfactor_[static_cast<uint32_t>(speed + 0.5f)]);
   return Cost(sec * factor, sec);
}

// Returns the time (in seconds) to make the transition from the predecessor
Cost BicycleCost::TransitionCost(const baldr::DirectedEdge* edge,
                               const baldr::NodeInfo* node,
                               const EdgeLabel& pred) const {
  // Special cases: gate, toll booth, false intersections
  if (edge->ctry_crossing()) {
    return { country_crossing_cost_ + country_crossing_penalty_,
             country_crossing_cost_ };
  } else if (node->type() == NodeType::kGate) {
    return { gate_cost_, gate_cost_ };
  } else if (node->intersection() == IntersectionType::kFalse) {
      return { 0.0f, 0.0f };
  } else {

    float penalty = 0.0f;
    if (!pred.destonly() && edge->destonly())
      penalty += destination_only_penalty_;

    if (pred.use() != Use::kAlley && edge->use() == Use::kAlley)
      penalty += alley_penalty_;

    // Transition cost = density * stopimpact * turncost + maneuverpenalty
    uint32_t idx = pred.opp_local_idx();
    float seconds = trans_density_factor_[node->density()] *
                    edge->stopimpact(idx) * TurnCost(edge->turntype(idx),
                         edge->edge_to_right(idx) && edge->edge_to_left(idx),
                         edge->drive_on_right());
    return (node->name_consistency(idx, edge->localedgeidx())) ?
              Cost(seconds + penalty, seconds) :
              Cost(seconds + maneuver_penalty_ + penalty, seconds);
  }
}

/**
 * Get the cost factor for A* heuristics. This factor is multiplied
 * with the distance to the destination to produce an estimate of the
 * minimum cost to the destination. The A* heuristic must underestimate the
 * cost to the destination. So a time based estimate based on speed should
 * assume the maximum speed is used to the destination such that the time
 * estimate is less than the least possible time along roads.
 */
float BicycleCost::AStarCostFactor() const {
  // Assume max speed of 80 kph (50 MPH)
  return speedfactor_[80];
}

// Compute a turn cost based on the turn type, crossing flag,
// and whether right or left side of road bicycling.
float BicycleCost::TurnCost(const baldr::Turn::Type turn_type,
                         const bool crossing,
                         const bool drive_on_right) const {
  if (crossing) {
    return kTCCrossing;
  }

  switch (turn_type) {
  case Turn::Type::kStraight:
    return kTCStraight;

  case Turn::Type::kSlightLeft:
  case Turn::Type::kSlightRight:
    return kTCSlight;

  case Turn::Type::kRight:
    return (drive_on_right) ? kTCFavorable : kTCUnfavorable;

  case Turn::Type::kLeft:
    return (drive_on_right) ? kTCUnfavorable : kTCFavorable;

  case Turn::Type::kSharpRight:
    return (drive_on_right) ? kTCFavorableSharp : kTCUnfavorableSharp;

  case Turn::Type::kSharpLeft:
    return (drive_on_right) ? kTCUnfavorableSharp : kTCFavorableSharp;

  case Turn::Type::kReverse:
    return kTCReverse;
  }
}

cost_ptr_t CreateBicycleCost(const boost::property_tree::ptree& config) {
  return std::make_shared<BicycleCost>(config);
}

}
}

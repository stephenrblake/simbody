/**@file
 *
 * Implementation of RigidBodyTree.
 * Note: there must be no mention of atoms anywhere in this code.
 */

#include "RigidBodyTree.h"
#include "RigidBodyNode.h"
#include "LengthConstraints.h"

#include <string>

SBState::~SBState() {
    delete vars;
    delete cache;
}

SBState::SBState(const SBState& src) : vars(0), cache(0) {
    if (src.vars)  vars  = new SimbodyTreeVariables(*src.vars);
    if (src.cache) cache = new SimbodyTreeResults(*src.cache);
}

SBState& SBState::operator=(const SBState& src) {
    if (&src != this) {
        delete vars; delete cache; vars = 0; cache = 0;
        if (src.vars)  vars  = new SimbodyTreeVariables(*src.vars);
        if (src.cache) cache = new SimbodyTreeResults(*src.cache);
    }
    return *this;
}


void RBStation::calcPosInfo(RBStationRuntime& rt) const {
    rt.station_G = getNode().getR_GB() * station_B;
    rt.pos_G     = getNode().getOB_G() + rt.station_G;
}

void RBStation::calcVelInfo(RBStationRuntime& rt) const {
    const Vec3& w_G = getNode().getSpatialAngVel();
    const Vec3& v_G = getNode().getSpatialLinVel();
    rt.stationVel_G = cross(w_G, rt.station_G);
    rt.vel_G = v_G + rt.stationVel_G;
}

void RBStation::calcAccInfo(RBStationRuntime& rt) const {
    const Vec3& w_G  = getNode().getSpatialAngVel();
    const Vec3& v_G  = getNode().getSpatialLinVel();
    const Vec3& aa_G = getNode().getSpatialAngAcc();
    const Vec3& a_G  = getNode().getSpatialLinAcc();
    rt.acc_G = a_G + cross(aa_G, rt.station_G)
                   + cross(w_G, rt.stationVel_G); // i.e., w X (wXr)
}

std::ostream& operator<<(std::ostream& o, const RBStation& s) {
    o << "station " << s.getStation() << " on node " << s.getNode().getNodeNum();
    return o;
}

void RBDistanceConstraint::calcPosInfo(RBDistanceConstraintRuntime& rt) const
{
    assert(isValid() && runtimeIndex >= 0);
    for (int i=0; i<=1; ++i) stations[i].calcPosInfo(rt.stationRuntimes[i]);

    rt.fromTip1ToTip2_G = rt.stationRuntimes[1].pos_G - rt.stationRuntimes[0].pos_G;
    const double separation = rt.fromTip1ToTip2_G.norm();
    rt.unitDirection_G = rt.fromTip1ToTip2_G / separation;
    rt.posErr = distance - separation;
}

void RBDistanceConstraint::calcVelInfo(RBDistanceConstraintRuntime& rt) const
{
    assert(isValid() && runtimeIndex >= 0);
    for (int i=0; i<=1; ++i) stations[i].calcVelInfo(rt.stationRuntimes[i]);

    rt.relVel_G = rt.stationRuntimes[1].vel_G - rt.stationRuntimes[0].vel_G;
    rt.velErr = ~rt.unitDirection_G * rt.relVel_G;
}

void RBDistanceConstraint::calcAccInfo(RBDistanceConstraintRuntime& rt) const
{
    assert(isValid() && runtimeIndex >= 0);
    for (int i=0; i<=1; ++i) stations[i].calcAccInfo(rt.stationRuntimes[i]);

//XXX this doesn't look right
    const Vec3 relAcc_G = rt.stationRuntimes[1].acc_G - rt.stationRuntimes[0].acc_G;
    rt.accErr = rt.relVel_G.normSqr() + (~relAcc_G * rt.fromTip1ToTip2_G);
}

RigidBodyTree::~RigidBodyTree() {
    delete lConstraints;

    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) {
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++) 
            delete rbNodeLevels[i][j];
        rbNodeLevels[i].resize(0);
    }
    rbNodeLevels.resize(0);
}

// Add a new node, taking over the heap space.
int RigidBodyTree::addRigidBodyNode(RigidBodyNode&      parent,
                                    const TransformMat& referenceConfig, // body frame in parent
                                    RigidBodyNode*&     nodep)
{
    RigidBodyNode* n = nodep; nodep=0;  // take ownership
    const int level = parent.getLevel() + 1;
    n->setLevel(level);

    // Put node in tree at the right level
    if ((int)rbNodeLevels.size()<=level) rbNodeLevels.resize(level+1);
    const int nxt = rbNodeLevels[level].size();
    rbNodeLevels[level].push_back(n);

    // Assign a unique reference integer to this node, for use by caller
    const int nodeNum = nodeNum2NodeMap.size();
    nodeNum2NodeMap.push_back(RigidBodyNodeIndex(level,nxt));
    n->setNodeNum(nodeNum);

    // Link in to the tree topology (bidirectional).
    parent.addChild(n, referenceConfig);

    return nodeNum;
}

// Add a new ground node. Must be first node added during construction.
void RigidBodyTree::addGroundNode() {
    // Make sure this is the first body
    assert(nodeNum2NodeMap.size() == 0);
    assert(rbNodeLevels.size() == 0);

    RigidBodyNode* n = 
        RigidBodyNode::create(MassProperties(), TransformMat(), Joint::ThisIsGround,
                              false, false, nextUSlot, nextQSlot);
    n->setLevel(0);

    // Put ground node in tree at level 0
    rbNodeLevels.resize(1);
    rbNodeLevels[0].push_back(n);
    nodeNum2NodeMap.push_back(RigidBodyNodeIndex(0,0));
    n->setNodeNum(0);
}

// Add a distance constraint and allocate slots to hold the runtime information for
// its stations. Return the assigned distance constraint index for caller's use.
int RigidBodyTree::addDistanceConstraint(const RBStation& s1, const RBStation& s2, const double& d)
{
    RBDistanceConstraint dc(s1,s2,d);
    dc.setRuntimeIndex(dcRuntimeInfo.size());
    dcRuntimeInfo.push_back(RBDistanceConstraintRuntime());
    distanceConstraints.push_back(dc);
    return distanceConstraints.size()-1;
}

// Here we lock in the topological structure of the multibody system,
// and compute allocation sizes for state variables.
void RigidBodyTree::realizeConstruction(const double& ctol, int verbose) {
    DOFTotal = SqDOFTotal = maxNQTotal = 0;
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++) {
            const int ndof = rbNodeLevels[i][j]->getDOF();
            DOFTotal += ndof; SqDOFTotal += ndof*ndof;
            maxNQTotal += rbNodeLevels[i][j]->getMaxNQ();
        }

    lConstraints = new LengthConstraints(*this, ctol, verbose);
    lConstraints->construct(distanceConstraints, dcRuntimeInfo);
}

// Here we lock in modeling choices like whether to use quaternions or Euler
// angles; what joints are prescribed, etc.
void RigidBodyTree::realizeModeling(const SBState& s) const {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->realizeModeling(s); 
}

// Here we lock in parameterization of the model, such as body masses.
void RigidBodyTree::realizeParameters(const SBState& s) const {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->realizeParameters(s); 
}

// Set generalized coordinates: sweep from base to tips.
void RigidBodyTree::realizeConfiguration(const Vector& pos)  {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->realizeConfiguration(pos); 

    for (size_t i=0; i < distanceConstraints.size(); ++i)
        distanceConstraints[i].calcPosInfo(
            dcRuntimeInfo[distanceConstraints[i].getRuntimeIndex()]);
}

// Set generalized speeds: sweep from base to tip.
// setPos() must have been called already.
void RigidBodyTree::realizeVelocity(const Vector& vel)  {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->realizeVelocity(vel); 

    for (size_t i=0; i < distanceConstraints.size(); ++i)
        distanceConstraints[i].calcVelInfo(
            dcRuntimeInfo[distanceConstraints[i].getRuntimeIndex()]);
}

// Enforce coordinate constraints -- order doesn't matter.
void RigidBodyTree::enforceTreeConstraints(Vector& pos, Vector& vel) {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++) 
            rbNodeLevels[i][j]->enforceConstraints(pos,vel);
}

// Enforce loop constraints.
void RigidBodyTree::enforceConstraints(Vector& pos, Vector& vel) {
    lConstraints->enforce(pos,vel); //FIX: previous constraints still obeyed? (CDS)
}


// Prepare for dynamics by calculating position-dependent quantities
// like the articulated body inertias P.
void RigidBodyTree::prepareForDynamics() {
    calcP();
}

// Given a set of spatial forces, calculate accelerations ignoring
// constraints. Must have already called prepareForDynamics().
// TODO: also applies stored internal forces (hinge torques) which
// will cause surprises if non-zero.
void RigidBodyTree::calcTreeForwardDynamics(const SpatialVecList& spatialForces) {
    calcZ(spatialForces);
    calcTreeAccel();
    
    for (size_t i=0; i < distanceConstraints.size(); ++i)
        distanceConstraints[i].calcAccInfo(
            dcRuntimeInfo[distanceConstraints[i].getRuntimeIndex()]);
}

// Given a set of spatial forces, calculate acclerations resulting from
// those forces and enforcement of acceleration constraints.
void RigidBodyTree::calcLoopForwardDynamics(const SpatialVecList& spatialForces) {
    SpatialVecList sFrc = spatialForces;
    calcTreeForwardDynamics(sFrc);
    if (lConstraints->calcConstraintForces()) {
        lConstraints->addInCorrectionForces(sFrc);
        calcTreeForwardDynamics(sFrc);
    }
}

// should be:
//   foreach tip {
//     traverse back to node which has more than one child hinge.
//   }
void RigidBodyTree::calcP() {
    // level 0 for atoms whose position is fixed
    for (int i=rbNodeLevels.size()-1 ; i>=0 ; i--) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->calcP();
}

// should be:
//   foreach tip {
//     traverse back to node which has more than one child hinge.
//   }
void RigidBodyTree::calcZ(const SpatialVecList& spatialForces) {
    // level 0 for atoms whose position is fixed
    for (int i=rbNodeLevels.size()-1 ; i>=0 ; i--) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++) {
            RigidBodyNode& node = *rbNodeLevels[i][j];
            node.calcZ(spatialForces[node.getNodeNum()]);
        }
}

// Y is used for length constraints: sweep from base to tip.
void RigidBodyTree::calcY() {
    for (int i=0; i < (int)rbNodeLevels.size(); i++)
        for (int j=0; j < (int)rbNodeLevels[i].size(); j++)
            rbNodeLevels[i][j]->calcY();
}

// Calc acceleration: sweep from base to tip.
void RigidBodyTree::calcTreeAccel() {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++)
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->calcAccel();
}

void RigidBodyTree::fixVel0(Vector& vel) {
    lConstraints->fixVel0(vel);
}

// Calc unconstrained internal forces from spatial forces: sweep from tip to base.
void RigidBodyTree::calcTreeInternalForces(const SpatialVecList& spatialForces) {
    for (int i=rbNodeLevels.size()-1 ; i>=0 ; i--)
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++) {
            RigidBodyNode& node = *rbNodeLevels[i][j];
            node.calcInternalForce(spatialForces[node.getNodeNum()]);
        }
}

// Retrieve already-computed internal forces (order doesn't matter).
void RigidBodyTree::getInternalForces(Vector& T) {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++)
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->getInternalForce(T);
}

void RigidBodyTree::getConstraintCorrectedInternalForces(Vector& T) {
    getInternalForces(T);
    lConstraints->fixGradient(T);
}

// Get current generalized coordinates (order doesn't matter).
void RigidBodyTree::getPos(Vector& pos) const {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->getPos(pos); 
}

// Get current generalized speeds (order doesn't matter).
void RigidBodyTree::getVel(Vector& vel) const {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++) 
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->getVel(vel); 
}

// Retrieve already-calculated accelerations (order doesn't matter)
void RigidBodyTree::getAcc(Vector& acc) const {
    for (int i=0 ; i<(int)rbNodeLevels.size() ; i++)
        for (int j=0 ; j<(int)rbNodeLevels[i].size() ; j++)
            rbNodeLevels[i][j]->getAccel(acc);
}

std::ostream& operator<<(std::ostream& o, const RigidBodyTree& tree) {
    o << "RigidBodyTree has " << tree.getNBodies() << " bodies (incl. G) in "
      << tree.rbNodeLevels.size() << " levels." << std::endl;
    o << "NodeNum->level,offset;stored nodeNum,level (stateOffset:dim)" << std::endl;
    for (int i=0; i < tree.getNBodies(); ++i) {
        o << i << "->" << tree.nodeNum2NodeMap[i].level << "," 
                       << tree.nodeNum2NodeMap[i].offset << ";";
        const RigidBodyNode& n = tree.getRigidBodyNode(i);
        o << n.getNodeNum() << "," << n.getLevel() 
          <<"(u"<< n.getUIndex()<<":"<<n.getDOF() 
          <<",q"<< n.getQIndex()<<":"<<n.getMaxNQ()<<")"<< std::endl;
    }

    return o;
}


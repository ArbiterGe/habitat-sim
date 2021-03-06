// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <Magnum/BulletIntegration/DebugDraw.h>
#include <Magnum/BulletIntegration/Integration.h>

#include "BulletCollision/CollisionShapes/btCompoundShape.h"
#include "BulletCollision/CollisionShapes/btConvexHullShape.h"
#include "BulletCollision/CollisionShapes/btConvexTriangleMeshShape.h"
#include "BulletCollision/Gimpact/btGImpactShape.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "BulletRigidObject.h"

//!  A Few considerations in construction
//!  Bullet Mesh conversion adapted from:
//!      https://github.com/mosra/magnum-integration/issues/20
//!      https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=11001
//!  Bullet object margin (p15):
//!      https://facultyfp.salisbury.edu/despickler/personal/Resources/
//!        GraphicsExampleCodeGLSL_SFML/InterfaceDoc/Bullet/Bullet_User_Manual.pdf
//!      It's okay to set margin down to 1mm
//!        (1) Bullet/MJCF example
//!      Another solution:
//!        (1) Keep 4cm margin
//!        (2) Use examples/Importers/ImportBsp

namespace esp {
namespace physics {

BulletRigidObject::BulletRigidObject(
    scene::SceneNode* rigidBodyNode,
    std::shared_ptr<btMultiBodyDynamicsWorld> bWorld)
    : RigidObject{rigidBodyNode},
      MotionState(*rigidBodyNode),
      bWorld_(bWorld) {}

bool BulletRigidObject::initializeSceneFinalize(
    const assets::ResourceManager& resMgr,
    const assets::PhysicsSceneAttributes::ptr physicsSceneAttributes,
    const std::vector<assets::CollisionMeshData>& meshGroup) {
  const assets::MeshMetaData& metaData =
      resMgr.getMeshMetaData(physicsSceneAttributes->getCollisionMeshHandle());

  constructBulletSceneFromMeshes(Magnum::Matrix4{}, meshGroup, metaData.root);
  for (auto& object : bSceneCollisionObjects_) {
    object->setFriction(physicsSceneAttributes->getFrictionCoefficient());
    object->setRestitution(physicsSceneAttributes->getRestitutionCoefficient());
    bWorld_->addCollisionObject(object.get());
  }

  return true;
}  // end BulletRigidObject::initializeScene

bool BulletRigidObject::initializeObjectFinalize(
    const assets::ResourceManager& resMgr,
    const assets::PhysicsObjectAttributes::ptr physicsObjectAttributes,
    const std::vector<assets::CollisionMeshData>& meshGroup) {
  objectMotionType_ = MotionType::DYNAMIC;

  const assets::MeshMetaData& metaData =
      resMgr.getMeshMetaData(physicsObjectAttributes->getCollisionMeshHandle());

  //! Physical parameters
  double margin = physicsObjectAttributes->getMargin();

  bool joinCollisionMeshes = physicsObjectAttributes->getJoinCollisionMeshes();

  usingBBCollisionShape_ = physicsObjectAttributes->getBoundingBoxCollisions();

  // TODO(alexanderwclegg): should provide the option for joinCollisionMeshes
  // and collisionFromBB_ to specify complete vs. component level bounding box
  // heirarchies.

  //! Iterate through all mesh components for one object
  //! The components are combined into a convex compound shape
  bObjectShape_ = std::make_unique<btCompoundShape>();

  if (!usingBBCollisionShape_) {
    constructBulletCompoundFromMeshes(Magnum::Matrix4{}, meshGroup,
                                      metaData.root, joinCollisionMeshes);

    // add the final object after joining meshes
    if (joinCollisionMeshes) {
      bObjectConvexShapes_.back()->setMargin(0.0);
      bObjectConvexShapes_.back()->recalcLocalAabb();
      bObjectShape_->addChildShape(btTransform::getIdentity(),
                                   bObjectConvexShapes_.back().get());
    }
  }

  //! Set properties
  bObjectShape_->setMargin(margin);

  Magnum::Vector3 objectScaling = physicsObjectAttributes->getScale();
  bObjectShape_->setLocalScaling(btVector3{objectScaling});

  btVector3 bInertia = btVector3(physicsObjectAttributes->getInertia());

  if (!usingBBCollisionShape_) {
    if (bInertia == btVector3{0, 0, 0}) {
      // allow bullet to compute the inertia tensor if we don't have one
      bObjectShape_->calculateLocalInertia(physicsObjectAttributes->getMass(),
                                           bInertia);  // overrides bInertia
      LOG(INFO) << "Automatic object inertia computed: " << bInertia.x() << " "
                << bInertia.y() << " " << bInertia.z();
    }
  }

  //! Bullet rigid body setup
  btRigidBody::btRigidBodyConstructionInfo info =
      btRigidBody::btRigidBodyConstructionInfo(
          physicsObjectAttributes->getMass(), &(btMotionState()),
          bObjectShape_.get(), bInertia);
  info.m_friction = physicsObjectAttributes->getFrictionCoefficient();
  info.m_restitution = physicsObjectAttributes->getRestitutionCoefficient();
  info.m_linearDamping = physicsObjectAttributes->getLinearDamping();
  info.m_angularDamping = physicsObjectAttributes->getAngularDamping();

  //! Create rigid body
  bObjectRigidBody_ = std::make_unique<btRigidBody>(info);
  //! Add to world
  bWorld_->addRigidBody(bObjectRigidBody_.get());
  //! Sync render pose with physics
  syncPose();
  return true;
}  // initializeObjectFinalize

void BulletRigidObject::finalizeObject() {
  if (isUsingBBCollisionShape()) {
    setCollisionFromBB();
  }
}
void BulletRigidObject::constructBulletSceneFromMeshes(
    const Magnum::Matrix4& transformFromParentToWorld,
    const std::vector<assets::CollisionMeshData>& meshGroup,
    const assets::MeshTransformNode& node) {
  Magnum::Matrix4 transformFromLocalToWorld =
      transformFromParentToWorld * node.transformFromLocalToParent;
  if (node.meshIDLocal != ID_UNDEFINED) {
    const assets::CollisionMeshData& mesh = meshGroup[node.meshIDLocal];

    // SCENE: create a concave static mesh
    btIndexedMesh bulletMesh;

    Corrade::Containers::ArrayView<Magnum::Vector3> v_data = mesh.positions;
    Corrade::Containers::ArrayView<Magnum::UnsignedInt> ui_data = mesh.indices;

    //! Configure Bullet Mesh
    //! This part is very likely to cause segfault, if done incorrectly
    bulletMesh.m_numTriangles = ui_data.size() / 3;
    bulletMesh.m_triangleIndexBase =
        reinterpret_cast<const unsigned char*>(ui_data.data());
    bulletMesh.m_triangleIndexStride = 3 * sizeof(Magnum::UnsignedInt);
    bulletMesh.m_numVertices = v_data.size();
    bulletMesh.m_vertexBase =
        reinterpret_cast<const unsigned char*>(v_data.data());
    bulletMesh.m_vertexStride = sizeof(Magnum::Vector3);
    bulletMesh.m_indexType = PHY_INTEGER;
    bulletMesh.m_vertexType = PHY_FLOAT;
    std::unique_ptr<btTriangleIndexVertexArray> indexedVertexArray =
        std::make_unique<btTriangleIndexVertexArray>();
    indexedVertexArray->addIndexedMesh(bulletMesh, PHY_INTEGER);  // exact shape

    //! Embed 3D mesh into bullet shape
    //! btBvhTriangleMeshShape is the most generic/slow choice
    //! which allows concavity if the object is static
    std::unique_ptr<btBvhTriangleMeshShape> meshShape =
        std::make_unique<btBvhTriangleMeshShape>(indexedVertexArray.get(),
                                                 true);
    meshShape->setMargin(0.0);
    meshShape->setLocalScaling(
        btVector3{transformFromLocalToWorld
                      .scaling()});  // scale is a property of the shape
    std::unique_ptr<btCollisionObject> sceneCollisionObject =
        std::make_unique<btCollisionObject>();
    sceneCollisionObject->setCollisionShape(meshShape.get());
    // rotation|translation are properties of the object
    sceneCollisionObject->setWorldTransform(
        btTransform{btMatrix3x3{transformFromLocalToWorld.rotation()},
                    btVector3{transformFromLocalToWorld.translation()}});

    bSceneArrays_.emplace_back(std::move(indexedVertexArray));
    bSceneShapes_.emplace_back(std::move(meshShape));
    bSceneCollisionObjects_.emplace_back(std::move(sceneCollisionObject));
  }

  for (auto& child : node.children) {
    constructBulletSceneFromMeshes(transformFromLocalToWorld, meshGroup, child);
  }
}

// recursively create the convex mesh shapes and add them to the compound in a
// flat manner by accumulating transformations down the tree
void BulletRigidObject::constructBulletCompoundFromMeshes(
    const Magnum::Matrix4& transformFromParentToWorld,
    const std::vector<assets::CollisionMeshData>& meshGroup,
    const assets::MeshTransformNode& node,
    bool join) {
  Magnum::Matrix4 transformFromLocalToWorld =
      transformFromParentToWorld * node.transformFromLocalToParent;
  if (node.meshIDLocal != ID_UNDEFINED) {
    // This node has a mesh, so add it to the compound

    const assets::CollisionMeshData& mesh = meshGroup[node.meshIDLocal];

    if (join) {
      // add all points to a single convex instead of compounding (more
      // stable)
      if (bObjectConvexShapes_.empty()) {
        // create the convex if it does not exist
        bObjectConvexShapes_.emplace_back(
            std::make_unique<btConvexHullShape>());
      }

      // add points
      for (auto& v : mesh.positions) {
        bObjectConvexShapes_.back()->addPoint(
            btVector3(transformFromLocalToWorld.transformPoint(v)), false);
      }
    } else {
      bObjectConvexShapes_.emplace_back(std::make_unique<btConvexHullShape>(
          static_cast<const btScalar*>(mesh.positions.data()->data()),
          mesh.positions.size(), sizeof(Magnum::Vector3)));
      bObjectConvexShapes_.back()->setMargin(0.0);
      bObjectConvexShapes_.back()->recalcLocalAabb();
      //! Add to compound shape stucture
      bObjectShape_->addChildShape(btTransform{transformFromLocalToWorld},
                                   bObjectConvexShapes_.back().get());
    }
  }

  for (auto& child : node.children) {
    constructBulletCompoundFromMeshes(transformFromLocalToWorld, meshGroup,
                                      child, join);
  }
}

void BulletRigidObject::setCollisionFromBB() {
  btVector3 dim(node().getCumulativeBB().size() / 2.0);

  for (auto& shape : bGenericShapes_) {
    bObjectShape_->removeChildShape(shape.get());
  }
  bGenericShapes_.clear();
  bGenericShapes_.emplace_back(std::make_unique<btBoxShape>(dim));
  bObjectShape_->addChildShape(btTransform::getIdentity(),
                               bGenericShapes_.back().get());
  bObjectShape_->recalculateLocalAabb();
  bObjectRigidBody_->setCollisionShape(bObjectShape_.get());

  if (bObjectRigidBody_->getInvInertiaDiagLocal() == btVector3{0, 0, 0}) {
    btVector3 bInertia(getInertiaVector());
    // allow bullet to compute the inertia tensor if we don't have one
    bObjectShape_->calculateLocalInertia(getMass(),
                                         bInertia);  // overrides bInertia

    LOG(INFO) << "Automatic BB object inertia computed: " << bInertia.x() << " "
              << bInertia.y() << " " << bInertia.z();

    setInertiaVector(Magnum::Vector3(bInertia));
  }
}

BulletRigidObject::~BulletRigidObject() {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ != MotionType::STATIC) {
    // remove rigid body from the world
    bWorld_->removeRigidBody(bObjectRigidBody_.get());
  } else if (rigidObjectType_ == RigidObjectType::SCENE ||
             objectMotionType_ == MotionType::STATIC) {
    // remove collision objects from the world
    for (auto& co : bSceneCollisionObjects_) {
      bWorld_->removeCollisionObject(co.get());
    }
  }
  bWorld_.reset();
}

bool BulletRigidObject::isActive() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return false;
  } else if (rigidObjectType_ == RigidObjectType::OBJECT) {
    return bObjectRigidBody_->isActive();
  } else {
    return false;
  }
}

void BulletRigidObject::setActive() {
  if (rigidObjectType_ == RigidObjectType::OBJECT) {
    bObjectRigidBody_->activate(true);
  }
}

bool BulletRigidObject::setMotionType(MotionType mt) {
  if (mt == objectMotionType_) {
    return true;  // no work
  }

  // remove the existing object from the world to change its type
  if (objectMotionType_ == MotionType::STATIC) {
    bWorld_->removeCollisionObject(bSceneCollisionObjects_.back().get());
    bSceneCollisionObjects_.clear();
  } else {
    bWorld_->removeRigidBody(bObjectRigidBody_.get());
  }

  if (rigidObjectType_ == RigidObjectType::OBJECT) {
    if (mt == MotionType::KINEMATIC) {
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() |
          btCollisionObject::CF_KINEMATIC_OBJECT);
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() &
          ~btCollisionObject::CF_STATIC_OBJECT);
      objectMotionType_ = MotionType::KINEMATIC;
      bWorld_->addRigidBody(bObjectRigidBody_.get());
      return true;
    } else if (mt == MotionType::STATIC) {
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() |
          btCollisionObject::CF_STATIC_OBJECT);
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() &
          ~btCollisionObject::CF_KINEMATIC_OBJECT);
      objectMotionType_ = MotionType::STATIC;

      // create a static scene collision object at the current transform
      std::unique_ptr<btCollisionObject> sceneCollisionObject =
          std::make_unique<btCollisionObject>();
      sceneCollisionObject->setCollisionShape(bObjectShape_.get());
      sceneCollisionObject->setWorldTransform(
          bObjectRigidBody_->getWorldTransform());
      bWorld_->addCollisionObject(
          sceneCollisionObject.get(),
          2,       // collisionFilterGroup (2 == StaticFilter)
          1 + 2);  // collisionFilterMask (1 == DefaultFilter, 2==StaticFilter)
      bSceneCollisionObjects_.emplace_back(std::move(sceneCollisionObject));
      return true;
    } else if (mt == MotionType::DYNAMIC) {
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() &
          ~btCollisionObject::CF_STATIC_OBJECT);
      bObjectRigidBody_->setCollisionFlags(
          bObjectRigidBody_->getCollisionFlags() &
          ~btCollisionObject::CF_KINEMATIC_OBJECT);
      objectMotionType_ = MotionType::DYNAMIC;
      bWorld_->addRigidBody(bObjectRigidBody_.get());
      setActive();
      return true;
    }
  }
  return false;
}

void BulletRigidObject::shiftOrigin(const Magnum::Vector3& shift) {
  Corrade::Utility::Debug() << "shiftOrigin: " << shift;

  if (visualNode_)
    visualNode_->translate(shift);

  // shift all children of the parent collision shape
  for (int i = 0; i < bObjectShape_->getNumChildShapes(); i++) {
    btTransform cT = bObjectShape_->getChildTransform(i);
    cT.setOrigin(cT.getOrigin() + btVector3(shift));
    bObjectShape_->updateChildTransform(i, cT, false);
  }
  // recompute the Aabb once when done
  bObjectShape_->recalculateLocalAabb();
  node().computeCumulativeBB();
}

void BulletRigidObject::applyForce(const Magnum::Vector3& force,
                                   const Magnum::Vector3& relPos) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->applyForce(btVector3(force), btVector3(relPos));
  }
}

void BulletRigidObject::setLinearVelocity(const Magnum::Vector3& linVel) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->setLinearVelocity(btVector3(linVel));
  }
}

void BulletRigidObject::setAngularVelocity(const Magnum::Vector3& angVel) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->setAngularVelocity(btVector3(angVel));
  }
}

Magnum::Vector3 BulletRigidObject::getLinearVelocity() const {
  return Magnum::Vector3{bObjectRigidBody_->getLinearVelocity()};
}

Magnum::Vector3 BulletRigidObject::getAngularVelocity() const {
  return Magnum::Vector3{bObjectRigidBody_->getAngularVelocity()};
}

void BulletRigidObject::applyImpulse(const Magnum::Vector3& impulse,
                                     const Magnum::Vector3& relPos) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->applyImpulse(btVector3(impulse), btVector3(relPos));
  }
}

//! Torque interaction
void BulletRigidObject::applyTorque(const Magnum::Vector3& torque) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->applyTorque(btVector3(torque));
  }
}

// Impulse Torque interaction
void BulletRigidObject::applyImpulseTorque(const Magnum::Vector3& impulse) {
  if (rigidObjectType_ == RigidObjectType::OBJECT &&
      objectMotionType_ == MotionType::DYNAMIC) {
    setActive();
    bObjectRigidBody_->applyTorqueImpulse(btVector3(impulse));
  }
}

//! Synchronize Physics transformations
//! Needed after changing the pose from Magnum side
void BulletRigidObject::syncPose() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    //! You shouldn't need to set scene transforms manually
    //! Scenes are loaded as is
    return;
  } else if (rigidObjectType_ == RigidObjectType::OBJECT) {
    //! For syncing objects
    bObjectRigidBody_->setWorldTransform(
        btTransform(node().transformationMatrix()));
  }
}

void BulletRigidObject::setMargin(const double margin) {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return;
  } else {
    for (std::size_t i = 0; i < bObjectConvexShapes_.size(); i++) {
      bObjectConvexShapes_[i]->setMargin(margin);
    }
    bObjectShape_->setMargin(margin);
  }
}

void BulletRigidObject::setMass(const double mass) {
  if (rigidObjectType_ == RigidObjectType::SCENE)
    return;
  else
    bObjectRigidBody_->setMassProps(mass, btVector3(getInertiaVector()));
}

void BulletRigidObject::setCOM(const Magnum::Vector3&) {
  // Current not supported
  /*if (rigidObjectType_ == RigidObjectType::SCENE)
    return;
  else
    bObjectRigidBody_->setCenterOfMassTransform(
        btTransform(Magnum::Matrix4<float>::translation(COM)));*/
}

void BulletRigidObject::setInertiaVector(const Magnum::Vector3& inertia) {
  if (rigidObjectType_ == RigidObjectType::SCENE)
    return;
  else
    bObjectRigidBody_->setMassProps(getMass(), btVector3(inertia));
}

void BulletRigidObject::setScale(const double) {
  // Currently not supported
  /*if (rigidObjectType_ == RigidObjectType::SCENE)
    return;
  else
    bObjectRigidBody_->setLinearFactor(btVector3(scale, scale, scale));*/
}

void BulletRigidObject::setFrictionCoefficient(
    const double frictionCoefficient) {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    for (std::size_t i = 0; i < bSceneCollisionObjects_.size(); i++) {
      bSceneCollisionObjects_[i]->setFriction(frictionCoefficient);
    }
  } else {
    bObjectRigidBody_->setFriction(frictionCoefficient);
  }
}

void BulletRigidObject::setRestitutionCoefficient(
    const double restitutionCoefficient) {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    for (std::size_t i = 0; i < bSceneCollisionObjects_.size(); i++) {
      bSceneCollisionObjects_[i]->setRestitution(restitutionCoefficient);
    }
  } else {
    bObjectRigidBody_->setRestitution(restitutionCoefficient);
  }
}

void BulletRigidObject::setLinearDamping(const double linearDamping) {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return;
  } else {
    bObjectRigidBody_->setDamping(linearDamping, getAngularDamping());
  }
}

void BulletRigidObject::setAngularDamping(const double angularDamping) {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return;
  } else {
    bObjectRigidBody_->setDamping(getLinearDamping(), angularDamping);
  }
}

double BulletRigidObject::getMargin() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return 0.0;
  } else {
    return bObjectShape_->getMargin();
  }
}

double BulletRigidObject::getMass() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return 0.0;
  } else {
    return 1.0 / bObjectRigidBody_->getInvMass();
  }
}

Magnum::Vector3 BulletRigidObject::getCOM() {
  // TODO: double check the position if there is any implicit transformation
  // done
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    const Magnum::Vector3 com = Magnum::Vector3();
    return com;
  } else {
    const Magnum::Vector3 com =
        Magnum::Vector3(bObjectRigidBody_->getCenterOfMassPosition());
    return com;
  }
}

Magnum::Vector3 BulletRigidObject::getInertiaVector() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    const Magnum::Vector3 inertia = Magnum::Vector3();
    return inertia;
  } else {
    const Magnum::Vector3 inertia =
        1.0 / Magnum::Vector3(bObjectRigidBody_->getInvInertiaDiagLocal());
    return inertia;
  }
}

Magnum::Matrix3 BulletRigidObject::getInertiaMatrix() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    const Magnum::Matrix3 inertia = Magnum::Matrix3();
    return inertia;
  } else {
    const Magnum::Vector3 vecInertia = getInertiaVector();
    const Magnum::Matrix3 inertia = Magnum::Matrix3::fromDiagonal(vecInertia);
    return inertia;
  }
}

double BulletRigidObject::getScale() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return 1.0;
    // Assume uniform scale for 3D objects
  } else {
    return bObjectRigidBody_->getLinearFactor().x();
  }
}

double BulletRigidObject::getFrictionCoefficient() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    if (bSceneCollisionObjects_.size() == 0) {
      return 0.0;
    } else {
      // Assume uniform friction in scene parts
      return bSceneCollisionObjects_.back()->getFriction();
    }
  } else {
    return bObjectRigidBody_->getFriction();
  }
}

double BulletRigidObject::getRestitutionCoefficient() {
  // Assume uniform restitution in scene parts
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    if (bSceneCollisionObjects_.size() == 0) {
      return 0.0;
    } else {
      return bSceneCollisionObjects_.back()->getRestitution();
    }
  } else {
    return bObjectRigidBody_->getRestitution();
  }
}

double BulletRigidObject::getLinearDamping() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return 0.0;
  } else {
    return bObjectRigidBody_->getLinearDamping();
  }
}

double BulletRigidObject::getAngularDamping() {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    return 0.0;
  } else {
    return bObjectRigidBody_->getAngularDamping();
  }
}

bool BulletRigidObject::contactTest() {
  SimulationContactResultCallback src;
  bWorld_->getCollisionWorld()->contactTest(bObjectRigidBody_.get(), src);
  return src.bCollision;
}

const Magnum::Range3D BulletRigidObject::getCollisionShapeAabb() const {
  if (rigidObjectType_ == RigidObjectType::SCENE) {
    Magnum::Range3D combinedAABB;
    // concatenate all component AABBs
    for (auto& object : bSceneCollisionObjects_) {
      btVector3 localAabbMin, localAabbMax;
      object->getCollisionShape()->getAabb(object->getWorldTransform(),
                                           localAabbMin, localAabbMax);
      if (combinedAABB == Magnum::Range3D{}) {
        // override an empty range instead of combining it
        combinedAABB = Magnum::Range3D{Magnum::Vector3{localAabbMin},
                                       Magnum::Vector3{localAabbMax}};
      } else {
        combinedAABB = Magnum::Math::join(
            combinedAABB, Magnum::Range3D{Magnum::Vector3{localAabbMin},
                                          Magnum::Vector3{localAabbMax}});
      }
    }
    return combinedAABB;
  }
  if (!bObjectShape_) {
    // e.g. empty scene
    return Magnum::Range3D();
  }
  btVector3 localAabbMin, localAabbMax;
  bObjectShape_->getAabb(btTransform::getIdentity(), localAabbMin,
                         localAabbMax);
  return Magnum::Range3D{Magnum::Vector3{localAabbMin},
                         Magnum::Vector3{localAabbMax}};
}

}  // namespace physics
}  // namespace esp

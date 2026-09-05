// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026.
//
#include "omniShadowCasterClassification.h"

#include <cmath>
#include <numbers>
#include <vector>

#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/light.h"
#include "scene/meshInstance.h"
#include "shadowCasterFiltering.h"

namespace visutwin::canvas
{
    void cullShadowCastersOmni(Light* light)
    {
        if (!light || light->type() != LightType::LIGHTTYPE_OMNI || !light->node()) {
            return;
        }

        LightRenderData* faces[6] = {};
        for (int face = 0; face < 6; ++face) {
            faces[face] = light->getRenderData(nullptr, face);
            if (!faces[face] || !faces[face]->shadowCamera) {
                return;
            }
            faces[face]->visibleCasters.clear();
        }

        // The six frusta share their near, far and field of view, so one camera's
        // parameters describe all of them.
        const Camera* shadowCamera = faces[0]->shadowCamera.get();
        const float nearClip = shadowCamera->nearClip();
        const float farClip = shadowCamera->farClip();
        const float slope = std::tan(shadowCamera->fov() * 0.5f *
            (std::numbers::pi_v<float> / 180.0f));

        // The union of the six frusta is bounded by an axis-aligned cube of this half
        // side. It is LARGER than the light's range: each face's far plane is flat and
        // perpendicular to its axis, so the frustum corners stick out past the range
        // sphere. Rejecting against a sphere here would drop casters sitting in those
        // corners, which do get lit.
        const float bounds = farClip * slope;

        // Light space is world space translated to put the light at the origin.
        const Vector3 lightPosition = light->node()->position();

        static thread_local std::vector<MeshInstance*> casters;
        casters.clear();
        collectShadowCasters(casters);

        for (auto* meshInstance : casters) {
            if (!meshInstance || !meshInstance->visible()) {
                continue;
            }
            if (!shouldRenderShadowMeshInstanceIgnoringVisibility(meshInstance)) {
                continue;
            }

            // A caster with culling switched off is in every face.
            if (!meshInstance->cull()) {
                for (auto* face : faces) {
                    face->visibleCasters.push_back(meshInstance);
                }
                meshInstance->setVisibleThisFrame(true);
                continue;
            }

            const BoundingBox& aabb = meshInstance->aabb();
            const Vector3 halfExtents = aabb.halfExtents();
            const Vector3 centre = aabb.center();
            const float ex = halfExtents.getX();
            const float ey = halfExtents.getY();
            const float ez = halfExtents.getZ();
            const float x = centre.getX() - lightPosition.getX();
            const float y = centre.getY() - lightPosition.getY();
            const float z = centre.getZ() - lightPosition.getZ();

            // Cheap rejection against the cube that bounds all six frusta.
            if (x > bounds + ex || x < -bounds - ex ||
                y > bounds + ey || y < -bounds - ey ||
                z > bounds + ez || z < -bounds - ez) {
                continue;
            }

            // A box is outside a plane when its signed distance is no greater than
            // minus its extent along that plane's normal. For a face with axial extent
            // ea and lateral extents eu, ev that reads: axial + ea > near,
            // axial - ea < far, and (slope * axial -+ lateral) > -(slope * ea + e).
            // The 1 / sqrt(1 + slope^2) that would normalize the side-plane normals
            // appears on both sides and is dropped.
            const float slopeX = slope * x;
            const float slopeY = slope * y;
            const float slopeZ = slope * z;

            // Side-plane limits, shared by the two faces of each axis.
            const float limXY = -(slope * ex + ey);
            const float limXZ = -(slope * ex + ez);
            const float limYX = -(slope * ey + ex);
            const float limYZ = -(slope * ey + ez);
            const float limZX = -(slope * ez + ex);
            const float limZY = -(slope * ez + ey);

            bool visible = false;

            // +X
            if (x + ex > nearClip && x - ex < farClip &&
                slopeX - y > limXY && slopeX + y > limXY &&
                slopeX - z > limXZ && slopeX + z > limXZ) {
                faces[0]->visibleCasters.push_back(meshInstance);
                visible = true;
            }
            // -X
            if (-x + ex > nearClip && -x - ex < farClip &&
                -slopeX - y > limXY && -slopeX + y > limXY &&
                -slopeX - z > limXZ && -slopeX + z > limXZ) {
                faces[1]->visibleCasters.push_back(meshInstance);
                visible = true;
            }
            // +Y
            if (y + ey > nearClip && y - ey < farClip &&
                slopeY - x > limYX && slopeY + x > limYX &&
                slopeY - z > limYZ && slopeY + z > limYZ) {
                faces[2]->visibleCasters.push_back(meshInstance);
                visible = true;
            }
            // -Y
            if (-y + ey > nearClip && -y - ey < farClip &&
                -slopeY - x > limYX && -slopeY + x > limYX &&
                -slopeY - z > limYZ && -slopeY + z > limYZ) {
                faces[3]->visibleCasters.push_back(meshInstance);
                visible = true;
            }
            // +Z
            if (z + ez > nearClip && z - ez < farClip &&
                slopeZ - x > limZX && slopeZ + x > limZX &&
                slopeZ - y > limZY && slopeZ + y > limZY) {
                faces[4]->visibleCasters.push_back(meshInstance);
                visible = true;
            }
            // -Z
            if (-z + ez > nearClip && -z - ez < farClip &&
                -slopeZ - x > limZX && -slopeZ + x > limZX &&
                -slopeZ - y > limZY && -slopeZ + y > limZY) {
                faces[5]->visibleCasters.push_back(meshInstance);
                visible = true;
            }

            if (visible) {
                meshInstance->setVisibleThisFrame(true);
            }
        }
    }
}

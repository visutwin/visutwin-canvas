// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
#include "animComponentProperties.h"

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/entity.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    bool readAnimComponentProperty(Entity* entity, const std::string& component, const std::string& property,
        std::array<float, 4>& value, int& components)
    {
        if (!entity) {
            return false;
        }
        if (component == "light") {
            auto* light = entity->findComponent<LightComponent>();
            if (!light) {
                return false;
            }
            if (property == "color") {
                const Color& c = light->color();
                value = {c.r, c.g, c.b, c.a};
                components = 4;
                return true;
            }
            components = 1;
            if (property == "intensity") {
                value[0] = light->intensity();
            } else if (property == "range") {
                value[0] = light->range();
            } else if (property == "innerConeAngle") {
                value[0] = light->innerConeAngle();
            } else if (property == "outerConeAngle") {
                value[0] = light->outerConeAngle();
            } else {
                return false;
            }
            return true;
        }
        if (component == "camera" && property == "fov") {
            auto* camera = entity->findComponent<CameraComponent>();
            if (!camera || !camera->camera()) {
                return false;
            }
            value[0] = camera->camera()->fov();
            components = 1;
            return true;
        }
        return false;
    }

    bool writeAnimComponentProperty(Entity* entity, const std::string& component, const std::string& property,
        const std::array<float, 4>& value, const int components)
    {
        if (!entity || components < 1) {
            return false;
        }
        if (component == "light") {
            auto* light = entity->findComponent<LightComponent>();
            if (!light) {
                return false;
            }
            if (property == "color") {
                // three components keep the light's alpha
                light->setColor(Color(value[0], value[1], value[2], components >= 4 ? value[3] : light->color().a));
            } else if (property == "intensity") {
                light->setIntensity(value[0]);
            } else if (property == "range") {
                light->setRange(value[0]);
            } else if (property == "innerConeAngle") {
                light->setInnerConeAngle(value[0]);
            } else if (property == "outerConeAngle") {
                light->setOuterConeAngle(value[0]);
            } else {
                return false;
            }
            return true;
        }
        if (component == "camera" && property == "fov") {
            auto* camera = entity->findComponent<CameraComponent>();
            if (!camera || !camera->camera()) {
                return false;
            }
            camera->camera()->setFov(value[0]);
            return true;
        }
        return false;
    }
}

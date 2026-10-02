// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Reading a material's generic parameters (Material::setParameter) by name, with the
// conversions every reader shares: an int read takes a float or a bool, a float read
// an integer, and so on. The packing in Material, the shader-variant options in
// ProgramLibrary and the cull mode in the renderer all read through here.
//
#pragma once

#include <cstdint>
#include <initializer_list>
#include <variant>

#include "scene/materials/material.h"

namespace visutwin::canvas
{
    /// The first of `names` the material has a parameter for, or null.
    inline const Material::ParameterValue* findMaterialParameter(const Material* material,
        std::initializer_list<const char*> names)
    {
        if (!material) {
            return nullptr;
        }
        // parameter() takes a std::string, so every name here would construct a
        // temporary — and the longer ones ("texture_metallicRoughnessMap") exceed the
        // small-string capacity and reach the heap. Materials that never call
        // setParameter have nothing to find, which is nearly all of them, and this runs
        // for each of them on every material switch.
        if (material->parameters().empty()) {
            return nullptr;
        }
        for (const char* name : names) {
            if (const auto* value = material->parameter(name)) {
                return value;
            }
        }
        return nullptr;
    }

    inline bool readParameterFloat(const Material::ParameterValue* value, float& out)
    {
        if (!value) {
            return false;
        }
        if (const auto* v = std::get_if<float>(value)) {
            out = *v;
            return true;
        }
        if (const auto* v = std::get_if<int32_t>(value)) {
            out = static_cast<float>(*v);
            return true;
        }
        if (const auto* v = std::get_if<uint32_t>(value)) {
            out = static_cast<float>(*v);
            return true;
        }
        return false;
    }

    inline bool readParameterInt(const Material::ParameterValue* value, int& out)
    {
        if (!value) {
            return false;
        }
        if (const auto* v = std::get_if<int32_t>(value)) {
            out = static_cast<int>(*v);
            return true;
        }
        if (const auto* v = std::get_if<uint32_t>(value)) {
            out = static_cast<int>(*v);
            return true;
        }
        if (const auto* v = std::get_if<float>(value)) {
            out = static_cast<int>(*v);
            return true;
        }
        if (const auto* v = std::get_if<bool>(value)) {
            out = *v ? 1 : 0;
            return true;
        }
        return false;
    }

    inline bool readParameterBool(const Material::ParameterValue* value, bool& out)
    {
        if (!value) {
            return false;
        }
        if (const auto* v = std::get_if<bool>(value)) {
            out = *v;
            return true;
        }
        if (const auto* v = std::get_if<int32_t>(value)) {
            out = *v != 0;
            return true;
        }
        if (const auto* v = std::get_if<uint32_t>(value)) {
            out = *v != 0u;
            return true;
        }
        if (const auto* v = std::get_if<float>(value)) {
            out = *v != 0.0f;
            return true;
        }
        return false;
    }

    /// A Color, Vector3 (alpha 1), Vector4 or a float (grey, alpha 1).
    inline bool readParameterColor4(const Material::ParameterValue* value, float out[4])
    {
        if (!value) {
            return false;
        }
        if (const auto* v = std::get_if<Color>(value)) {
            out[0] = v->r;
            out[1] = v->g;
            out[2] = v->b;
            out[3] = v->a;
            return true;
        }
        if (const auto* v = std::get_if<Vector3>(value)) {
            out[0] = v->getX();
            out[1] = v->getY();
            out[2] = v->getZ();
            out[3] = 1.0f;
            return true;
        }
        if (const auto* v = std::get_if<Vector4>(value)) {
            out[0] = v->getX();
            out[1] = v->getY();
            out[2] = v->getZ();
            out[3] = v->getW();
            return true;
        }
        if (const auto* v = std::get_if<float>(value)) {
            out[0] = *v;
            out[1] = *v;
            out[2] = *v;
            out[3] = 1.0f;
            return true;
        }
        return false;
    }

    inline bool readParameterTexture(const Material::ParameterValue* value, Texture*& out)
    {
        if (!value) {
            return false;
        }
        if (const auto* v = std::get_if<Texture*>(value)) {
            out = *v;
            return true;
        }
        return false;
    }
}

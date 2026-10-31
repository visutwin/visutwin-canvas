// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.07.2026
//

#include <iostream>

#include "platform/graphics/blendState.h"

using namespace visutwin::canvas;

namespace
{
    bool fail(const std::string& message)
    {
        std::cerr << "FAILED: " << message << "\n";
        return false;
    }

    // Every field must survive a round trip, and no field may bleed into its neighbours. The
    // packed layout was re-laid-out when the dual-source factors pushed each factor from 4 to 5
    // bits, so this pins the shifts down.
    bool checkFieldIndependence()
    {
        BlendState state;
        state.setEnabled(true);
        state.setColorOp(BLENDEQUATION_REVERSE_SUBTRACT);
        state.setColorSrcFactor(BLENDMODE_ONE_MINUS_SRC1_ALPHA);   // 16 - needs the 5th bit
        state.setColorDstFactor(BLENDMODE_SRC1_COLOR);             // 13
        state.setAlphaOp(BLENDEQUATION_MAX);
        state.setAlphaSrcFactor(BLENDMODE_ONE_MINUS_SRC1_COLOR);   // 14
        state.setAlphaDstFactor(BLENDMODE_SRC1_ALPHA);             // 15
        state.setRedWrite(true);
        state.setGreenWrite(false);
        state.setBlueWrite(true);
        state.setAlphaWrite(false);

        if (!state.enabled()) return fail("enabled did not round trip");
        if (state.colorOp() != BLENDEQUATION_REVERSE_SUBTRACT) return fail("colorOp did not round trip");
        if (state.colorSrcFactor() != BLENDMODE_ONE_MINUS_SRC1_ALPHA) {
            return fail("colorSrcFactor did not round trip (factor 16 needs 5 bits), got " +
                std::to_string(state.colorSrcFactor()));
        }
        if (state.colorDstFactor() != BLENDMODE_SRC1_COLOR) return fail("colorDstFactor did not round trip");
        if (state.alphaOp() != BLENDEQUATION_MAX) return fail("alphaOp did not round trip");
        if (state.alphaSrcFactor() != BLENDMODE_ONE_MINUS_SRC1_COLOR) return fail("alphaSrcFactor did not round trip");
        if (state.alphaDstFactor() != BLENDMODE_SRC1_ALPHA) return fail("alphaDstFactor did not round trip");
        if (!state.redWrite() || state.greenWrite() || !state.blueWrite() || state.alphaWrite()) {
            return fail("colour write mask did not round trip");
        }

        // Rewriting one field must not disturb any other.
        state.setColorSrcFactor(BLENDMODE_ZERO);
        if (state.colorDstFactor() != BLENDMODE_SRC1_COLOR || state.colorOp() != BLENDEQUATION_REVERSE_SUBTRACT ||
            state.alphaSrcFactor() != BLENDMODE_ONE_MINUS_SRC1_COLOR || state.alphaDstFactor() != BLENDMODE_SRC1_ALPHA ||
            state.alphaOp() != BLENDEQUATION_MAX || !state.enabled()) {
            return fail("writing colorSrcFactor disturbed a neighbouring field");
        }

        return true;
    }

    // Each factor slot must independently drive the dual-source query.
    bool checkUsesDualSourceBlending()
    {
        if (BlendState().usesDualSourceBlending()) {
            return fail("a default blend state must not report dual-source");
        }
        if (BlendState::alphaBlend().usesDualSourceBlending() ||
            BlendState::additiveBlend().usesDualSourceBlending() ||
            BlendState::multiplicativeBlend().usesDualSourceBlending()) {
            return fail("a standard preset must not report dual-source");
        }

        const int dualFactors[] = {
            BLENDMODE_SRC1_COLOR, BLENDMODE_ONE_MINUS_SRC1_COLOR,
            BLENDMODE_SRC1_ALPHA, BLENDMODE_ONE_MINUS_SRC1_ALPHA
        };
        for (const int factor : dualFactors) {
            BlendState a; a.setColorSrcFactor(factor);
            BlendState b; b.setColorDstFactor(factor);
            BlendState c; c.setAlphaSrcFactor(factor);
            BlendState d; d.setAlphaDstFactor(factor);
            if (!a.usesDualSourceBlending() || !b.usesDualSourceBlending() ||
                !c.usesDualSourceBlending() || !d.usesDualSourceBlending()) {
                return fail("factor " + std::to_string(factor) + " not detected in every slot");
            }
        }

        // The factor just below the dual-source range must not trip it.
        BlendState boundary;
        boundary.setColorSrcFactor(BLENDMODE_ONE_MINUS_CONSTANT);
        if (boundary.usesDualSourceBlending()) {
            return fail("BLENDMODE_ONE_MINUS_CONSTANT wrongly reported as dual-source");
        }

        return true;
    }

    // The presets are what every existing material relies on; the repacking must not shift them.
    bool checkPresets()
    {
        const BlendState alpha = BlendState::alphaBlend();
        if (!alpha.enabled() || alpha.colorOp() != BLENDEQUATION_ADD ||
            alpha.colorSrcFactor() != BLENDMODE_SRC_ALPHA ||
            alpha.colorDstFactor() != BLENDMODE_ONE_MINUS_SRC_ALPHA ||
            alpha.alphaSrcFactor() != BLENDMODE_ONE ||
            alpha.alphaDstFactor() != BLENDMODE_ONE_MINUS_SRC_ALPHA) {
            return fail("alphaBlend preset changed");
        }

        const BlendState mul = BlendState::multiplicativeBlend();
        if (mul.colorSrcFactor() != BLENDMODE_DST_COLOR || mul.colorDstFactor() != BLENDMODE_ZERO) {
            return fail("multiplicativeBlend preset changed");
        }

        const BlendState add = BlendState::additiveBlend();
        if (add.colorSrcFactor() != BLENDMODE_SRC_ALPHA || add.colorDstFactor() != BLENDMODE_ONE) {
            return fail("additiveBlend preset changed");
        }

        // Distinct states must hash distinctly - key() feeds the pipeline cache, so a collision
        // would hand back a pipeline built for different blending.
        if (alpha.key() == mul.key() || alpha.key() == add.key() || mul.key() == add.key()) {
            return fail("preset keys collide");
        }
        BlendState dual = BlendState::alphaBlend();
        dual.setColorDstFactor(BLENDMODE_ONE_MINUS_SRC1_ALPHA);
        if (dual.key() == alpha.key()) {
            return fail("a dual-source variant collides with its non-dual-source original");
        }

        return true;
    }
}

namespace
{
    // Alpha to coverage and the per-attachment states are what a pipeline adds to its key: each
    // must move the key, and none may leak into another attachment or field.
    bool checkCoverageAndAttachments()
    {
        BlendState base = BlendState::alphaBlend();
        const uint32_t baseKey = base.key();
        if (base.alphaToCoverage() || base.hasAttachmentStates() || base.attachmentsKey() != 0) {
            return fail("a new blend state has alpha to coverage or attachment states");
        }

        BlendState coverage = base;
        coverage.setAlphaToCoverage(true);
        if (!coverage.alphaToCoverage() || coverage.key() == baseKey || !coverage.enabled() ||
            coverage.colorDstFactor() != base.colorDstFactor()) {
            return fail("alpha to coverage must move the key and leave every other field");
        }
        coverage.setAlphaToCoverage(false);
        if (coverage.key() != baseKey) {
            return fail("turning alpha to coverage off must restore the key");
        }

        // An attachment without a state of its own takes attachment 0's, without its coverage.
        BlendState withCoverage = base;
        withCoverage.setAlphaToCoverage(true);
        const BlendState inherited = withCoverage.attachment(2);
        if (inherited.alphaToCoverage() || inherited.colorSrcFactor() != base.colorSrcFactor() ||
            !inherited.enabled()) {
            return fail("an attachment without a state takes attachment 0's, alpha to coverage cleared");
        }

        BlendState additiveRed;
        additiveRed.setEnabled(true);
        additiveRed.setColorSrcFactor(BLENDMODE_ONE);
        additiveRed.setColorDstFactor(BLENDMODE_ONE);
        additiveRed.setGreenWrite(false);
        additiveRed.setBlueWrite(false);
        additiveRed.setAlphaWrite(false);
        additiveRed.setAlphaToCoverage(true);

        BlendState independent = base;
        independent.setAttachment(1, additiveRed);
        const BlendState first = independent.attachment(1);
        if (!independent.hasAttachmentStates() || independent.attachmentsKey() == 0 ||
            independent.key() != baseKey) {
            return fail("an attachment state goes in attachmentsKey, attachment 0's key is unchanged");
        }
        if (first.colorDstFactor() != BLENDMODE_ONE || first.greenWrite() || !first.redWrite() ||
            first.alphaToCoverage()) {
            return fail("attachment 1 reads back its own state, alpha to coverage cleared");
        }
        if (independent.attachment(0).key() != baseKey || independent.attachment(2).colorDstFactor() !=
            base.colorDstFactor()) {
            return fail("attachments 0 and 2 keep attachment 0's state");
        }

        BlendState other = base;
        other.setAttachment(2, additiveRed);
        if (other.attachmentsKey() == independent.attachmentsKey()) {
            return fail("the same state on another attachment must key differently");
        }
        BlendState ignored = base;
        ignored.setAttachment(0, additiveRed);
        ignored.setAttachment(4, additiveRed);
        if (ignored.hasAttachmentStates() || ignored.key() != baseKey) {
            return fail("attachments outside 1-3 are ignored");
        }
        return true;
    }
}

int main()
{
    if (!checkFieldIndependence() || !checkUsesDualSourceBlending() || !checkPresets() ||
        !checkCoverageAndAttachments()) {
        return 1;
    }

    std::cout << "blend state tests passed\n";
    return 0;
}

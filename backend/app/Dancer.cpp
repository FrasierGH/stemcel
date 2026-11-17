#include "Dancer.h"

#include <cmath>

namespace stemcel::native
{
namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;

double nowSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

// A beat's push: strongest on the beat, gone by its end.
float kick(double phase)
{
    const auto t = static_cast<float>(phase - std::floor(phase));
    return (1.0F - t) * (1.0F - t);
}
} // namespace

Dancer::Dancer()
{
    setSize(96, 124);
    setRepaintsOnMouseActivity(false);
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    setTooltip("Sprig dances to what plays. Drag to move; double-click for another dance; right-click to hide");
    startedAt = nowSeconds();
    startTimerHz(60);
}

Dancer::~Dancer() = default;

void Dancer::timerCallback()
{
    // Out of sight while Stemcel's window is minimised, back with it.
    if (isOnDesktop() && ownerMinimised)
    {
        const bool minimised = ownerMinimised();
        if (minimised && isVisible())
        {
            hiddenWithOwner = true;
            setVisible(false);
        }
        else if (!minimised && hiddenWithOwner)
        {
            hiddenWithOwner = false;
            setVisible(true);
        }
    }
    if (!isShowing()) return;
    tick();
    repaint();
}

void Dancer::tick()
{
    if (beatSource) last = beatSource();
    // The energy follows the level: quickly up, slowly down, nothing when stopped.
    const float target = last.playing ? juce::jlimit(0.0F, 1.0F, 0.35F + last.level * 0.9F) : 0.0F;
    energy += (target - energy) * (target > energy ? 0.25F : 0.03F);
}

Dancer::Pose Dancer::poseAt(const Beat& beat, double seconds) const
{
    Pose p;
    const float e = beat.playing ? juce::jmax(energy, 0.35F) : 0.0F;
    // Idle: a slow sway and a breath.
    const auto idle = static_cast<float>(std::sin(seconds * 2.0 * juce::MathConstants<double>::pi * 0.4));
    p.lean = idle * 0.05F * (1.0F - e);
    p.sprout = idle * 0.25F;
    p.squash = static_cast<float>(std::sin(seconds * 2.0 * juce::MathConstants<double>::pi * 0.25)) * 0.03F;
    // A blink every few seconds.
    const double blinkPhase = std::fmod(seconds, 3.7);
    p.blink = blinkPhase < 0.12;
    if (!beat.playing) return p;

    const double b = beat.beats;
    const auto beatIndex = static_cast<long long>(std::floor(b));
    const float k = kick(b);
    const auto t = static_cast<float>(b - std::floor(b));
    const bool even = (beatIndex % 2) == 0;
    p.squash = k * 0.22F * e;                                  // lands on the beat
    p.hop = std::sin(kPi * t) * 0.16F * e;                     // and springs up between
    p.sprout = std::sin(kPi * static_cast<float>(b)) * 0.45F * e; // the sprout flicks each beat
    const auto phrase = static_cast<int>(((beatIndex % 16) + 16) % 16);
    switch (dance)
    {
        case 0: // arms up in turn, stepping
            p.armL = (even ? 2.4F * (1.0F - t * 0.5F) : 0.5F) * e;
            p.armR = (even ? 0.5F : 2.4F * (1.0F - t * 0.5F)) * e;
            p.stepL = even ? std::sin(kPi * t) * e : 0.0F;
            p.stepR = even ? 0.0F : std::sin(kPi * t) * e;
            p.lean = (even ? -0.12F : 0.12F) * e;
            break;
        case 1: // both arms pumping, a jump on the bar
            p.armL = p.armR = (1.6F + std::sin(kPi * 2.0F * t) * 0.9F) * e;
            if (beatIndex % 4 == 0) p.hop = std::sin(kPi * t) * 0.42F * e;
            p.stepL = p.stepR = k * 0.3F * e;
            break;
        default: // the wave: arms swing through, hips sway
            p.armL = (1.4F + std::sin(kPi * static_cast<float>(b)) * 1.1F) * e;
            p.armR = (1.4F - std::sin(kPi * static_cast<float>(b)) * 1.1F) * e;
            p.lean = std::sin(kPi * static_cast<float>(b)) * 0.18F * e;
            p.stepL = std::max(0.0F, std::sin(kPi * static_cast<float>(b))) * 0.6F * e;
            p.stepR = std::max(0.0F, -std::sin(kPi * static_cast<float>(b))) * 0.6F * e;
            break;
    }
    // Into each new phrase (every 4 bars): a spin over its first beat.
    if (phrase == 0 && e > 0.5F) p.spin = std::cos(t * 2.0F * kPi);
    return p;
}

void Dancer::paint(juce::Graphics& g)
{
    const auto pose = poseAt(last, nowSeconds() - startedAt);
    const auto bounds = getLocalBounds().toFloat();
    const float unit = juce::jmin(bounds.getWidth() / 96.0F, bounds.getHeight() / 124.0F); // drawn on a 96 x 124 grid
    const float groundY = bounds.getBottom() - 6.0F * unit;
    const float cx = bounds.getCentreX();
    const juce::Colour body = colours::accent, leaf = colours::emerald500.brighter(0.15F), dark = juce::Colours::black;
    const juce::Colour outline = body.darker(0.6F);

    // The shadow, smaller as it hops.
    g.setColour(juce::Colours::black.withAlpha(0.25F));
    g.fillEllipse(juce::Rectangle<float>(0.0F, 0.0F, (34.0F - pose.hop * 30.0F) * unit, 6.0F * unit).withCentre({cx, groundY}));

    // Legs: two stems down to the feet, a lifted foot raised.
    const float hipY = groundY - (24.0F + pose.hop * 60.0F) * unit;
    g.setColour(outline);
    for (int side = 0; side < 2; ++side)
    {
        const float dir = side == 0 ? -1.0F : 1.0F;
        const float lift = (side == 0 ? pose.stepL : pose.stepR) * 10.0F * unit;
        const juce::Point<float> hip{cx + dir * 7.0F * unit, hipY};
        const juce::Point<float> foot{cx + dir * 11.0F * unit, groundY - 2.0F * unit - lift - pose.hop * 50.0F * unit};
        g.drawLine({hip, foot}, 3.0F * unit);
        g.fillEllipse(juce::Rectangle<float>(9.0F * unit, 4.5F * unit).withCentre(foot.translated(dir * 3.0F * unit, 0.0F)));
    }

    // The body: a seed, squashed as it lands, leaning and turning with the dance.
    const float bodyW = 40.0F * unit * (1.0F + pose.squash * 0.6F), bodyH = 46.0F * unit * (1.0F - pose.squash);
    const juce::Point<float> centre{cx, hipY - bodyH * 0.42F};
    const auto turn = juce::AffineTransform::scale(pose.spin, 1.0F, centre.x, centre.y).rotated(pose.lean, cx, hipY);
    juce::Path seed;
    seed.addEllipse(juce::Rectangle<float>(bodyW, bodyH).withCentre(centre));

    // Arms: leaves on short stems, swung from the shoulders (behind the body when it turns).
    auto drawArm = [&](float side, float angle)
    {
        const juce::Point<float> shoulder{centre.x + side * bodyW * 0.42F, centre.y + bodyH * 0.05F};
        juce::Path arm;
        arm.startNewSubPath(0.0F, 0.0F);
        arm.lineTo(0.0F, 14.0F * unit);
        juce::Path blade;
        blade.addEllipse(-4.5F * unit, 12.0F * unit, 9.0F * unit, 15.0F * unit);
        const auto place = juce::AffineTransform::rotation(-side * angle).translated(shoulder).followedBy(turn);
        g.setColour(outline);
        g.strokePath(arm, juce::PathStrokeType(2.5F * unit, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), place);
        g.setColour(leaf);
        g.fillPath(blade, place);
        g.setColour(leaf.darker(0.5F));
        g.strokePath(blade, juce::PathStrokeType(1.0F * unit), place);
    };
    drawArm(-1.0F, pose.armL);
    drawArm(1.0F, pose.armR);

    g.setColour(body);
    g.fillPath(seed, turn);
    // A highlight, so it reads as round.
    juce::Path shine;
    shine.addEllipse(juce::Rectangle<float>(bodyW * 0.3F, bodyH * 0.22F).withCentre(centre.translated(-bodyW * 0.18F, -bodyH * 0.24F)));
    g.setColour(juce::Colours::white.withAlpha(0.28F));
    g.fillPath(shine, turn);
    g.setColour(outline);
    g.strokePath(seed, juce::PathStrokeType(2.0F * unit), turn);

    // The sprout on top: a stem and two leaves, flicked by each beat.
    const juce::Point<float> crown{centre.x, centre.y - bodyH * 0.5F};
    {
        juce::Path stem;
        stem.startNewSubPath(0.0F, 0.0F);
        stem.quadraticTo(3.0F * unit, -8.0F * unit, 0.0F, -14.0F * unit);
        juce::Path leaves;
        leaves.addEllipse(-13.0F * unit, -21.0F * unit, 12.0F * unit, 7.0F * unit);
        leaves.addEllipse(1.0F * unit, -19.0F * unit, 12.0F * unit, 7.0F * unit);
        const auto place = juce::AffineTransform::rotation(pose.sprout).translated(crown).followedBy(turn);
        g.setColour(outline);
        g.strokePath(stem, juce::PathStrokeType(2.0F * unit), place);
        g.setColour(leaf);
        g.fillPath(leaves, place);
    }

    // The face (only from the front: it turns away mid-spin).
    if (pose.spin > 0.25F)
    {
        const auto face = juce::AffineTransform::scale(pose.spin, 1.0F, centre.x, centre.y).rotated(pose.lean, cx, hipY);
        g.setColour(dark);
        for (const float side : {-1.0F, 1.0F})
        {
            juce::Path eye;
            const auto at = centre.translated(side * bodyW * 0.17F, -bodyH * 0.06F);
            if (pose.blink) eye.addRectangle(juce::Rectangle<float>(7.0F * unit, 1.6F * unit).withCentre(at));
            else
            {
                eye.addEllipse(juce::Rectangle<float>(6.0F * unit, 8.0F * unit).withCentre(at));
                g.fillPath(eye, face);
                juce::Path glint;
                glint.addEllipse(juce::Rectangle<float>(2.2F * unit, 2.2F * unit).withCentre(at.translated(1.2F * unit, -1.8F * unit)));
                g.setColour(juce::Colours::white);
                g.fillPath(glint, face);
                g.setColour(dark);
                continue;
            }
            g.fillPath(eye, face);
        }
        // A smile that opens wider the harder it dances.
        juce::Path mouth;
        const auto m = centre.translated(0.0F, bodyH * 0.16F);
        const float open = 2.0F + energy * 4.0F;
        mouth.startNewSubPath(m.x - 6.0F * unit, m.y);
        mouth.quadraticTo(m.x, m.y + open * unit, m.x + 6.0F * unit, m.y);
        g.strokePath(mouth, juce::PathStrokeType(1.8F * unit, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), face);
        g.setColour(juce::Colour(0xffff6f91).withAlpha(0.45F));
        for (const float side : {-1.0F, 1.0F})
            g.fillEllipse(juce::Rectangle<float>(6.0F * unit, 3.5F * unit).withCentre(centre.translated(side * bodyW * 0.3F, bodyH * 0.1F)).transformedBy(face));
    }
}

void Dancer::mouseDown(const juce::MouseEvent& e)
{
    dragged = false;
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Another dance");
        menu.addItem(3, "Stay on top", true, stayOnTop);
        menu.addItem(2, "Hide Sprig");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safe = juce::Component::SafePointer<Dancer>(this)](int chosen)
                           {
                               if (safe == nullptr) return;
                               if (chosen == 1) safe->setStyle(safe->dance + 1);
                               if (chosen == 2 && safe->onHide) safe->onHide();
                               if (chosen == 3)
                               {
                                   safe->setStayOnTop(!safe->stayOnTop);
                                   if (safe->onStayOnTop) safe->onStayOnTop(safe->stayOnTop);
                               }
                           });
        return;
    }
    dragger.startDraggingComponent(this, e);
}

void Dancer::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    dragged = true;
    // Anywhere on screen, as long as enough of it stays in view to grab again.
    juce::ComponentBoundsConstrainer keepGrabbable;
    keepGrabbable.setMinimumOnscreenAmounts(getHeight() / 2, getWidth() / 2, getHeight() / 2, getWidth() / 2);
    dragger.dragComponent(this, e, &keepGrabbable);
}

void Dancer::mouseUp(const juce::MouseEvent& /*event*/)
{
    if (dragged && onMoved) onMoved(getPosition());
}

void Dancer::mouseDoubleClick(const juce::MouseEvent& /*event*/)
{
    setStyle(dance + 1);
}

} // namespace stemcel::native

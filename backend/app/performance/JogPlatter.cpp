#include "JogPlatter.h"

#include "Theme.h"

#include <cmath>

namespace stemcel::native::perf
{

void JogPlatter::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float size = std::min(bounds.getWidth(), bounds.getHeight()) - 4.0F;
    if (size < 24.0F) return;
    const auto disc = juce::Rectangle<float>(size, size).withCentre(bounds.getCentre());
    const auto centre = disc.getCentre();
    const float radius = size * 0.5F;
    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

    // The ring: how far through the track (accent; amber for its last 30 seconds), the cue
    // point on it.
    const float ringWidth = std::max(3.0F, radius * 0.07F);
    juce::Path ring;
    ring.addCentredArc(centre.x, centre.y, radius - ringWidth * 0.5F, radius - ringWidth * 0.5F, 0.0F, 0.0F, kTwoPi, true);
    g.setColour(colours::zinc800);
    g.strokePath(ring, juce::PathStrokeType(ringWidth));
    if (s.loaded && s.lengthSec > 0.0)
    {
        const auto done = static_cast<float>(juce::jlimit(0.0, 1.0, position / s.lengthSec));
        juce::Path elapsed;
        elapsed.addCentredArc(centre.x, centre.y, radius - ringWidth * 0.5F, radius - ringWidth * 0.5F, 0.0F, 0.0F, done * kTwoPi, true);
        const bool ending = s.lengthSec - position < 30.0;
        g.setColour(ending ? colours::amber400 : colours::accent);
        g.strokePath(elapsed, juce::PathStrokeType(ringWidth));
        const auto cueAngle = static_cast<float>(s.cueSec / s.lengthSec) * kTwoPi;
        const auto tip = centre.getPointOnCircumference(radius - ringWidth * 0.5F, cueAngle);
        g.setColour(colours::red500);
        g.fillEllipse(juce::Rectangle<float>(ringWidth * 1.4F, ringWidth * 1.4F).withCentre(tip));
    }

    // The record: dark vinyl with grooves, a glow while touched.
    const float vinyl = radius - ringWidth - 3.0F;
    const auto record = juce::Rectangle<float>(vinyl * 2.0F, vinyl * 2.0F).withCentre(centre);
    g.setColour(s.touched ? colours::zinc700 : colours::grey900);
    g.fillEllipse(record);
    g.setColour(juce::Colours::white.withAlpha(0.05F));
    for (float r = vinyl * 0.45F; r < vinyl - 2.0F; r += std::max(2.5F, vinyl * 0.06F))
        g.drawEllipse(juce::Rectangle<float>(r * 2.0F, r * 2.0F).withCentre(centre), 1.0F);
    if (s.touched)
    {
        g.setColour(colours::accent.withAlpha(0.6F));
        g.drawEllipse(record, 2.0F);
    }

    // It turns at 33⅓ rpm with the track: the angle is where the track is.
    const auto angle = static_cast<float>(std::fmod(position * (100.0 / 3.0) / 60.0, 1.0)) * kTwoPi;
    const auto from = centre.getPointOnCircumference(vinyl * 0.36F, angle);
    const auto to = centre.getPointOnCircumference(vinyl - 2.0F, angle);
    g.setColour(s.loaded ? juce::Colours::white : colours::textFaint);
    g.drawLine(juce::Line<float>(from, to), std::max(2.0F, vinyl * 0.035F));

    // The label: the deck number, the label's colour lit while it plays.
    const float label = vinyl * 0.34F;
    g.setColour(s.playing ? colours::accent.withAlpha(0.85F) : colours::zinc700);
    g.fillEllipse(juce::Rectangle<float>(label * 2.0F, label * 2.0F).withCentre(centre));
    g.setColour(s.playing ? colours::onAccent : colours::textDim);
    g.setFont(uiFont(std::max(10.0F, label * 0.9F), true));
    g.drawText(juce::String(deck), juce::Rectangle<float>(label * 2.0F, label * 2.0F).withCentre(centre), juce::Justification::centred);
}

} // namespace stemcel::native::perf

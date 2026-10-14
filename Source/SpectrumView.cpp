#include "SpectrumView.h"

#include <algorithm>
#include <cmath>

namespace
{
const juce::Colour backgroundColour (0xff121418);
const juce::Colour gridColour (0xff2a2e36);
const juce::Colour gridStrongColour (0xff3a404b);
const juce::Colour labelColour (0xff8a93a3);
constexpr float minFreq = 20.0f;
constexpr float maxFreq = 20000.0f;
} // namespace

SpectrumView::SpectrumView()
{
    setOpaque (true);
}

float SpectrumView::curvePointFrequency (int index) noexcept
{
    const float t = static_cast<float> (index) / static_cast<float> (kCurvePoints - 1);
    return minFreq * std::pow (maxFreq / minFreq, t);
}

juce::Rectangle<float> SpectrumView::getPlotArea() const
{
    auto area = getLocalBounds().toFloat();
    area.removeFromLeft (38.0f);
    area.removeFromRight (42.0f);
    area.removeFromTop (8.0f);
    area.removeFromBottom (20.0f);

    if (area.getWidth() < 10.0f || area.getHeight() < 10.0f)
        return { 0.0f, 0.0f, 10.0f, 10.0f };

    return area;
}

float SpectrumView::freqToX (float freq) const
{
    const auto area = getPlotArea();
    const float f = juce::jlimit (minFreq, maxFreq, std::isfinite (freq) ? freq : 1000.0f);
    return area.getX() + area.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
}

float SpectrumView::xToFreq (float x) const
{
    const auto area = getPlotArea();
    const float t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return minFreq * std::pow (maxFreq / minFreq, t);
}

float SpectrumView::eqDbToY (float db) const
{
    const auto area = getPlotArea();
    const float d = std::isfinite (db) ? db : 0.0f;
    const float y = area.getCentreY() - d / eqRangeDb * area.getHeight() * 0.5f;
    return juce::jlimit (area.getY() - 4.0f, area.getBottom() + 4.0f, y);
}

float SpectrumView::yToEqDb (float y) const
{
    const auto area = getPlotArea();
    return (area.getCentreY() - y) / (area.getHeight() * 0.5f) * eqRangeDb;
}

float SpectrumView::specDbToY (float db) const
{
    const auto area = getPlotArea();
    const float d = juce::jlimit (spectrumFloorDb, 0.0f, std::isfinite (db) ? db : spectrumFloorDb);
    return area.getY() + (0.0f - d) / (0.0f - spectrumFloorDb) * area.getHeight();
}

//==============================================================================
void SpectrumView::setEqRange (float rangeDb)
{
    const float r = juce::jlimit (3.0f, 48.0f, rangeDb);

    if (std::abs (r - eqRangeDb) > 0.001f)
    {
        eqRangeDb = r;
        rebuildCurve();
        repaintRequested = true;
    }
}

void SpectrumView::setSpectrumFloor (float floorDb)
{
    const float f = juce::jlimit (-160.0f, -24.0f, floorDb);

    if (std::abs (f - spectrumFloorDb) > 0.001f)
    {
        spectrumFloorDb = f;
        rebuildAll();
    }
}

void SpectrumView::beginTraceUpdate()
{
    for (auto& t : traces)
        t.touched = false;
}

void SpectrumView::updateTrace (std::uint64_t owner, juce::Colour colour, bool selected, const eqtrum::SpectrumSeqlock& source)
{
    auto it = std::find_if (traces.begin(), traces.end(), [owner] (const Trace& t) { return t.owner == owner; });

    if (it == traces.end())
    {
        traces.emplace_back();
        traces.back().owner = owner;
        it = traces.end() - 1;
    }

    auto& trace = *it;
    trace.touched = true;

    if (trace.colour != colour || trace.selected != selected)
    {
        trace.colour = colour;
        trace.selected = selected;
        repaintRequested = true;
    }

    const auto version = source.getVersion();

    if (version != trace.version)
    {
        std::array<float, eqtrum::kNumSpectrumBins> tmp;

        if (source.read (tmp.data()))
        {
            trace.bins = tmp;
            trace.version = version;
            trace.hasData = true;
            rebuildTrace (trace);
            repaintRequested = true;
        }
    }
}

void SpectrumView::endTraceUpdate()
{
    const auto before = traces.size();
    traces.erase (std::remove_if (traces.begin(), traces.end(), [] (const Trace& t) { return ! t.touched; }), traces.end());

    if (traces.size() != before)
        repaintRequested = true;
}

void SpectrumView::updateLine (Line& line, const eqtrum::SpectrumSeqlock* source, std::uint64_t key)
{
    if (source == nullptr)
    {
        if (line.active)
        {
            line.active = false;
            line.hasData = false;
            line.path.clear();
            repaintRequested = true;
        }

        return;
    }

    if (! line.active || line.key != key)
    {
        line.active = true;
        line.key = key;
        line.version = 0xffffffffu;
        line.hasData = false;
    }

    const auto version = source->getVersion();

    if (version != line.version)
    {
        std::array<float, eqtrum::kNumSpectrumBins> tmp;

        if (source->read (tmp.data()))
        {
            line.bins = tmp;
            line.version = version;
            line.hasData = true;
            rebuildLine (line);
            repaintRequested = true;
        }
    }
}

void SpectrumView::updateSumSpectrum (const eqtrum::SpectrumSeqlock* source)
{
    updateLine (sumLine, source, 1);
}

void SpectrumView::updateSidechainSpectrum (const eqtrum::SpectrumSeqlock* source, std::uint64_t key)
{
    updateLine (sidechainLine, source, key);
}

void SpectrumView::setEqCurve (const std::vector<float>& curveDb, juce::Colour colour, bool editable)
{
    curve = curveDb;
    curveColour = colour;
    curveEditable = editable;
    hasCurve = ! curve.empty();
    rebuildCurve();
    repaintRequested = true;
}

void SpectrumView::clearEqCurve()
{
    if (hasCurve)
    {
        hasCurve = false;
        curve.clear();
        curvePath.clear();
        repaintRequested = true;
    }
}

void SpectrumView::setNodes (const std::array<Node, eqtrum::kNumBands>& newNodes, int selectedBand, bool show)
{
    bool changed = show != showNodes || selectedBand != selectedNode;

    for (size_t i = 0; i < nodes.size() && ! changed; ++i)
    {
        const auto& a = nodes[i];
        const auto& b = newNodes[i];
        changed = a.on != b.on || a.hasGain != b.hasGain
               || std::abs (a.freq - b.freq) > 0.01f
               || std::abs (a.gainDb - b.gainDb) > 0.001f
               || std::abs (a.dynOffsetDb - b.dynOffsetDb) > 0.01f;
    }

    nodes = newNodes;
    selectedNode = selectedBand;
    showNodes = show;

    if (changed)
        repaintRequested = true;
}

void SpectrumView::setMessage (const juce::String& text)
{
    if (text != message)
    {
        message = text;
        repaintRequested = true;
    }
}

bool SpectrumView::consumeRepaintRequest() noexcept
{
    const bool r = repaintRequested;
    repaintRequested = false;
    return r;
}

//==============================================================================
void SpectrumView::rebuildTrace (Trace& trace)
{
    trace.fill.clear();
    trace.line.clear();

    if (! trace.hasData)
        return;

    const auto area = getPlotArea();
    const int n = eqtrum::kNumSpectrumBins;
    trace.line.preallocateSpace (n * 3 + 8);

    for (int i = 0; i < n; ++i)
    {
        const float x = area.getX() + area.getWidth() * static_cast<float> (i) / static_cast<float> (n - 1);
        const float y = specDbToY (trace.bins[static_cast<size_t> (i)]);

        if (i == 0)
            trace.line.startNewSubPath (x, y);
        else
            trace.line.lineTo (x, y);
    }

    trace.fill = trace.line;
    trace.fill.lineTo (area.getRight(), area.getBottom());
    trace.fill.lineTo (area.getX(), area.getBottom());
    trace.fill.closeSubPath();
}

void SpectrumView::rebuildLine (Line& line)
{
    line.path.clear();

    if (! line.hasData)
        return;

    const auto area = getPlotArea();
    const int n = eqtrum::kNumSpectrumBins;
    line.path.preallocateSpace (n * 3 + 4);

    for (int i = 0; i < n; ++i)
    {
        const float x = area.getX() + area.getWidth() * static_cast<float> (i) / static_cast<float> (n - 1);
        const float y = specDbToY (line.bins[static_cast<size_t> (i)]);

        if (i == 0)
            line.path.startNewSubPath (x, y);
        else
            line.path.lineTo (x, y);
    }
}

void SpectrumView::rebuildCurve()
{
    curvePath.clear();

    if (curve.empty())
        return;

    for (size_t i = 0; i < curve.size(); ++i)
    {
        const float x = freqToX (curvePointFrequency (static_cast<int> (i)));
        const float y = eqDbToY (curve[i]);

        if (i == 0)
            curvePath.startNewSubPath (x, y);
        else
            curvePath.lineTo (x, y);
    }
}

void SpectrumView::rebuildAll()
{
    for (auto& t : traces)
        rebuildTrace (t);

    rebuildLine (sumLine);
    rebuildLine (sidechainLine);
    rebuildCurve();
    repaintRequested = true;
}

void SpectrumView::resized()
{
    rebuildAll();
    repaint();
}

//==============================================================================
void SpectrumView::drawGrid (juce::Graphics& g)
{
    const auto area = getPlotArea();
    g.setFont (juce::FontOptions (11.0f));

    const float gridFreqs[] = { 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f,
                                200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 700.0f, 800.0f, 900.0f, 1000.0f,
                                2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f, 10000.0f };

    for (const float f : gridFreqs)
    {
        const bool strong = f == 100.0f || f == 1000.0f || f == 10000.0f;
        g.setColour (strong ? gridStrongColour : gridColour);
        g.drawVerticalLine (juce::roundToInt (freqToX (f)), area.getY(), area.getBottom());
    }

    const float labelFreqs[] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    g.setColour (labelColour);

    for (const float f : labelFreqs)
    {
        const juce::String text = f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String (juce::roundToInt (f));
        const float x = freqToX (f);
        g.drawText (text, juce::Rectangle<float> (x - 20.0f, area.getBottom() + 2.0f, 40.0f, 16.0f),
                    juce::Justification::centred, false);
    }

    // EQ dB scale (left)
    const float step = eqRangeDb <= 6.0f ? 2.0f : (eqRangeDb <= 12.0f ? 3.0f : (eqRangeDb <= 24.0f ? 6.0f : 10.0f));

    for (float db = -eqRangeDb; db <= eqRangeDb + 0.01f; db += step)
    {
        const float y = eqDbToY (db);
        g.setColour (std::abs (db) < 0.01f ? gridStrongColour : gridColour);
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (labelColour);
        const juce::String text = (db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db));
        g.drawText (text, juce::Rectangle<float> (2.0f, y - 8.0f, 34.0f, 16.0f), juce::Justification::centredRight, false);
    }

    // spectrum dBFS scale (right)
    const float specStep = spectrumFloorDb >= -60.0f ? 12.0f : 24.0f;
    g.setColour (labelColour.withAlpha (0.8f));

    for (float db = 0.0f; db >= spectrumFloorDb - 0.01f; db -= specStep)
    {
        const float y = specDbToY (db);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (area.getRight() + 4.0f, y - 8.0f, 36.0f, 16.0f),
                    juce::Justification::centredLeft, false);
    }

    g.setColour (gridStrongColour);
    g.drawRect (area, 1.0f);
}

juce::Point<float> SpectrumView::getNodePosition (int band) const
{
    const auto& n = nodes[static_cast<size_t> (band)];
    return { freqToX (n.freq), eqDbToY (n.hasGain ? n.gainDb : 0.0f) };
}

int SpectrumView::hitTestNode (juce::Point<float> position) const
{
    if (! showNodes)
        return -1;

    int best = -1;
    float bestDistance = 12.0f;

    for (int b = 0; b < eqtrum::kNumBands; ++b)
    {
        const float d = getNodePosition (b).getDistanceFrom (position);

        if (d < bestDistance)
        {
            bestDistance = d;
            best = b;
        }
    }

    return best;
}

void SpectrumView::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
    drawGrid (g);

    const auto area = getPlotArea();

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());

        for (const auto& t : traces)
        {
            if (t.selected || ! t.hasData)
                continue;

            g.setColour (t.colour.withAlpha (0.10f));
            g.fillPath (t.fill);
            g.setColour (t.colour.withAlpha (0.35f));
            g.strokePath (t.line, juce::PathStrokeType (1.0f));
        }

        for (const auto& t : traces)
        {
            if (! t.selected || ! t.hasData)
                continue;

            g.setColour (t.colour.withAlpha (0.25f));
            g.fillPath (t.fill);
            g.setColour (t.colour.withAlpha (0.9f));
            g.strokePath (t.line, juce::PathStrokeType (1.2f));
        }

        if (sumLine.active && sumLine.hasData)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.strokePath (sumLine.path, juce::PathStrokeType (1.5f));
        }

        if (sidechainLine.active && sidechainLine.hasData)
        {
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, sidechainLine.path, dashes, 2);
            g.setColour (juce::Colour (0xffffeb3b).withAlpha (0.85f));
            g.fillPath (dashed);
        }

        if (hasCurve)
        {
            g.setColour (curveColour.withAlpha (curveEditable ? 1.0f : 0.6f));
            g.strokePath (curvePath, juce::PathStrokeType (curveEditable ? 2.2f : 1.5f));
        }
    }

    if (showNodes)
    {
        g.setFont (juce::FontOptions (10.0f));

        for (int b = 0; b < eqtrum::kNumBands; ++b)
        {
            const auto& n = nodes[static_cast<size_t> (b)];
            const auto pos = getNodePosition (b);
            const bool selected = b == selectedNode;

            if (n.hasGain && std::abs (n.dynOffsetDb) > 0.05f && n.on)
            {
                g.setColour (juce::Colour (0xffffa726));
                g.drawLine (pos.x, pos.y, pos.x, eqDbToY (n.gainDb + n.dynOffsetDb), 3.0f);
            }

            const float radius = selected ? 7.5f : 6.0f;
            const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (pos);
            const auto colour = n.on ? curveColour : juce::Colour (0xff6b7280);

            g.setColour (colour.withAlpha (n.on ? 0.9f : 0.5f));

            if (n.on)
                g.fillEllipse (circle);
            else
                g.drawEllipse (circle, 1.5f);

            if (selected)
            {
                g.setColour (juce::Colours::white);
                g.drawEllipse (circle.expanded (2.5f), 1.5f);
            }

            g.setColour (juce::Colours::white.withAlpha (0.85f));
            const juce::String label = b == 0 ? "HP" : (b == 1 ? "LP" : (b == 2 ? "LS" : (b == 3 ? "HS" : juce::String (b - 3))));
            g.drawText (label, juce::Rectangle<float> (pos.x - 15.0f, pos.y - radius - 15.0f, 30.0f, 12.0f),
                        juce::Justification::centred, false);
        }
    }

    if (message.isNotEmpty())
    {
        g.setColour (labelColour);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (message, area, juce::Justification::centred, true);
    }
}

//==============================================================================
void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    draggingNode = -1;

    if (! curveEditable)
        return;

    const int band = hitTestNode (e.position);

    if (band < 0)
        return;

    draggingNode = band;

    if (onNodeSelected != nullptr)
        onNodeSelected (band);

    if (onNodeDragStart != nullptr)
        onNodeDragStart (band);
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingNode < 0 || onNodeDrag == nullptr)
        return;

    const float freq = xToFreq (e.position.x);
    const float gain = juce::jlimit (-18.0f, 18.0f, yToEqDb (e.position.y));
    onNodeDrag (draggingNode, freq, gain);
}

void SpectrumView::mouseUp (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);

    if (draggingNode >= 0 && onNodeDragEnd != nullptr)
        onNodeDragEnd (draggingNode);

    draggingNode = -1;
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! curveEditable)
        return;

    const int band = hitTestNode (e.position);

    if (band >= 0 && onNodeDoubleClick != nullptr)
        onNodeDoubleClick (band);
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! curveEditable || onNodeWheel == nullptr)
        return;

    int band = hitTestNode (e.position);

    if (band < 0)
        band = selectedNode;

    if (band >= 0)
        onNodeWheel (band, wheel.deltaY);
}

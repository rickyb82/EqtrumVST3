#pragma once

#include "InstanceRegistry.h"
#include "Params.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// Log-frequency graph: overlaid track spectra, HUB sum line, sidechain spectrum, EQ curve and nodes.
// Paths are rebuilt only when new data arrives (version change), on resize or on range change.
class SpectrumView : public juce::Component
{
public:
    struct Node
    {
        bool on = false;
        bool hasGain = false;
        float freq = 1000.0f;
        float gainDb = 0.0f;
        float dynOffsetDb = 0.0f;
    };

    SpectrumView();

    std::function<void (int band)> onNodeSelected;
    std::function<void (int band)> onNodeDragStart;
    std::function<void (int band, float freqHz, float gainDb)> onNodeDrag;
    std::function<void (int band)> onNodeDragEnd;
    std::function<void (int band)> onNodeDoubleClick;
    std::function<void (int band, float wheelDelta)> onNodeWheel;

    void setEqRange (float rangeDb);
    void setSpectrumFloor (float floorDb);

    void beginTraceUpdate();
    void updateTrace (std::uint64_t owner, juce::Colour colour, bool selected, const eqtrum::SpectrumSeqlock& source);
    void endTraceUpdate();

    void updateSumSpectrum (const eqtrum::SpectrumSeqlock* source);                           // nullptr hides
    void updateSidechainSpectrum (const eqtrum::SpectrumSeqlock* source, std::uint64_t key);  // nullptr hides

    void setEqCurve (const std::vector<float>& curveDb, juce::Colour colour, bool editable);
    void clearEqCurve();
    void setNodes (const std::array<Node, eqtrum::kNumBands>& newNodes, int selectedBand, bool show);
    void setMessage (const juce::String& text);

    bool consumeRepaintRequest() noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    static constexpr int kCurvePoints = 256;
    static float curvePointFrequency (int index) noexcept;

private:
    struct Trace
    {
        std::uint64_t owner = 0;
        juce::Colour colour;
        bool selected = false;
        bool touched = false;
        bool hasData = false;
        std::uint32_t version = 0xffffffffu;
        std::array<float, eqtrum::kNumSpectrumBins> bins {};
        juce::Path fill;
        juce::Path line;
    };

    struct Line
    {
        bool active = false;
        bool hasData = false;
        std::uint64_t key = 0;
        std::uint32_t version = 0xffffffffu;
        std::array<float, eqtrum::kNumSpectrumBins> bins {};
        juce::Path path;
    };

    juce::Rectangle<float> getPlotArea() const;
    float freqToX (float freq) const;
    float xToFreq (float x) const;
    float eqDbToY (float db) const;
    float yToEqDb (float y) const;
    float specDbToY (float db) const;

    void rebuildTrace (Trace& trace);
    void rebuildLine (Line& line);
    void rebuildCurve();
    void rebuildAll();
    void updateLine (Line& line, const eqtrum::SpectrumSeqlock* source, std::uint64_t key);
    juce::Point<float> getNodePosition (int band) const;
    int hitTestNode (juce::Point<float> position) const;
    void drawGrid (juce::Graphics& g);

    float eqRangeDb = 18.0f;
    float spectrumFloorDb = -90.0f;
    std::vector<Trace> traces;
    Line sumLine;
    Line sidechainLine;

    std::vector<float> curve;
    juce::Path curvePath;
    juce::Colour curveColour { juce::Colours::white };
    bool curveEditable = false;
    bool hasCurve = false;

    std::array<Node, eqtrum::kNumBands> nodes;
    int selectedNode = -1;
    bool showNodes = false;
    int draggingNode = -1;
    juce::String message;
    bool repaintRequested = true;
};

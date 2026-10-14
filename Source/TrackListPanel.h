#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Side list of TRACK instances: editable name, colour, show/hide, solo-view, selection.
class TrackListPanel : public juce::Component
{
public:
    struct RowInfo
    {
        std::uint64_t owner = 0;
        juce::String name;
        juce::Colour colour;
        bool visible = true;
        bool solo = false;
        bool selected = false;
        bool editable = false;   // name and colour can be changed from this editor
        bool isSelf = false;
    };

    TrackListPanel();
    ~TrackListPanel() override;

    std::function<void (std::uint64_t owner)> onSelect;
    std::function<void (std::uint64_t owner, const juce::String& name)> onRename;
    std::function<void (std::uint64_t owner, juce::Colour colour)> onColour;
    std::function<void (std::uint64_t owner, bool visible)> onVisible;
    std::function<void (std::uint64_t owner, bool solo)> onSolo;

    void setRows (const std::vector<RowInfo>& newRows);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class Row;

    void layoutRows();

    juce::Label title;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
    int numActiveRows = 0;
};

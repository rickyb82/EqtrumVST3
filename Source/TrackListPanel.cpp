#include "TrackListPanel.h"

#include "InstanceRegistry.h"

#include <algorithm>

namespace
{
constexpr int rowHeight = 28;
constexpr int maxRows = 64;
} // namespace

//==============================================================================
class TrackListPanel::Row : public juce::Component
{
public:
    explicit Row (TrackListPanel& ownerPanel) : panel (ownerPanel)
    {
        nameLabel.setFont (juce::FontOptions (13.0f));
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        nameLabel.addMouseListener (this, false);
        nameLabel.onTextChange = [this]
        {
            if (info.editable && panel.onRename != nullptr)
                panel.onRename (info.owner, nameLabel.getText());
        };
        addAndMakeVisible (nameLabel);

        visibleButton.setTooltip ("Mostra / nascondi lo spettro");
        visibleButton.onClick = [this]
        {
            if (panel.onVisible != nullptr)
                panel.onVisible (info.owner, visibleButton.getToggleState());
        };
        addAndMakeVisible (visibleButton);

        soloButton.setTooltip ("Solo-view: mostra solo le tracce in solo");
        soloButton.onClick = [this]
        {
            if (panel.onSolo != nullptr)
                panel.onSolo (info.owner, soloButton.getToggleState());
        };
        addAndMakeVisible (soloButton);
    }

    ~Row() override
    {
        nameLabel.removeMouseListener (this);
    }

    void update (const RowInfo& newInfo)
    {
        const bool needsRepaint = newInfo.colour != info.colour || newInfo.selected != info.selected
                               || newInfo.isSelf != info.isSelf;
        info = newInfo;

        nameLabel.setEditable (false, info.editable, false);

        if (! nameLabel.isBeingEdited())
        {
            if (nameLabel.getText() != info.name)
                nameLabel.setText (info.name, juce::dontSendNotification);
        }

        visibleButton.setToggleState (info.visible, juce::dontSendNotification);
        soloButton.setToggleState (info.solo, juce::dontSendNotification);

        if (needsRepaint)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);

        if (info.selected)
        {
            g.setColour (juce::Colour (0xff2d3442));
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (info.colour.withAlpha (0.9f));
            g.drawRoundedRectangle (bounds, 4.0f, 1.2f);
        }

        g.setColour (info.colour);
        g.fillRoundedRectangle (swatchArea.toFloat(), 3.0f);

        if (info.isSelf)
        {
            g.setColour (juce::Colours::white);
            g.fillEllipse (swatchArea.toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 3);
        swatchArea = r.removeFromLeft (16).withSizeKeepingCentre (16, 16);
        r.removeFromLeft (6);
        soloButton.setBounds (r.removeFromRight (36));
        visibleButton.setBounds (r.removeFromRight (36));
        nameLabel.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto pos = e.getEventRelativeTo (this).getPosition();

        if (swatchArea.contains (pos) && info.editable)
        {
            showColourMenu();
            return;
        }

        if (panel.onSelect != nullptr)
            panel.onSelect (info.owner);
    }

private:
    void showColourMenu()
    {
        juce::PopupMenu menu;

        for (int i = 0; i < eqtrum::getPaletteSize(); ++i)
            menu.addColouredItem (i + 1, "Colore " + juce::String (i + 1), eqtrum::getPaletteColour (i));

        juce::Component::SafePointer<Row> safeThis (this);
        const auto owner = info.owner;

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safeThis, owner] (int result)
                            {
                                if (safeThis == nullptr || result <= 0)
                                    return;

                                if (safeThis->panel.onColour != nullptr)
                                    safeThis->panel.onColour (owner, eqtrum::getPaletteColour (result - 1));
                            });
    }

    TrackListPanel& panel;
    RowInfo info;
    juce::Label nameLabel;
    juce::ToggleButton visibleButton { "V" };
    juce::ToggleButton soloButton { "S" };
    juce::Rectangle<int> swatchArea;
};

//==============================================================================
TrackListPanel::TrackListPanel()
{
    title.setText ("Tracce", juce::dontSendNotification);
    title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (title);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    for (int i = 0; i < maxRows; ++i)
    {
        rows.push_back (std::make_unique<Row> (*this));
        content.addChildComponent (*rows.back());
    }
}

TrackListPanel::~TrackListPanel()
{
    viewport.setViewedComponent (nullptr, false);
}

void TrackListPanel::setRows (const std::vector<RowInfo>& newRows)
{
    const int count = std::min (static_cast<int> (newRows.size()), maxRows);

    for (int i = 0; i < maxRows; ++i)
    {
        auto& row = rows[static_cast<size_t> (i)];

        if (i < count)
        {
            row->update (newRows[static_cast<size_t> (i)]);
            row->setVisible (true);
        }
        else
        {
            row->setVisible (false);
        }
    }

    if (count != numActiveRows)
    {
        numActiveRows = count;
        layoutRows();
    }
}

void TrackListPanel::layoutRows()
{
    const int width = std::max (40, viewport.getWidth() - viewport.getScrollBarThickness());
    content.setSize (width, std::max (1, numActiveRows * rowHeight));

    for (int i = 0; i < maxRows; ++i)
        rows[static_cast<size_t> (i)]->setBounds (0, i * rowHeight, width, rowHeight);
}

void TrackListPanel::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff181b21));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void TrackListPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    title.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    viewport.setBounds (r);
    layoutRows();
}

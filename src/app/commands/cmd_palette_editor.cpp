// Aseprite
// Copyright (C) 2001-2015  David Capello
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/cmd_sequence.h"
#include "app/cmd/set_palette.h"
#include "app/color.h"
#include "app/color_utils.h"
#include "app/commands/command.h"
#include "app/commands/params.h"
#include "app/console.h"
#include "app/context_access.h"
#include "app/document_undo.h"
#include "app/file_selector.h"
#include "app/ini_file.h"
#include "app/modules/editors.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/settings/settings.h"
#include "app/settings/settings.h"
#include "app/transaction.h"
#include "app/ui/color_bar.h"
#include "app/ui/color_sliders.h"
#include "app/ui/editor/editor.h"
#include "app/ui/hex_color_entry.h"
#include "app/ui/palette_view.h"
#include "app/ui/skin/skin_slider_property.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui_context.h"
#include "base/bind.h"
#include "base/fs.h"
#include "base/path.h"
#include "doc/image.h"
#include "doc/palette.h"
#include "doc/sprite.h"
#include "gfx/hsv.h"
#include "gfx/rgb.h"
#include "gfx/size.h"
#include "ui/graphics.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace app {

using namespace gfx;
using namespace ui;

class PaletteEntryEditor : public Window {
public:
  PaletteEntryEditor();

  void setColor(const app::Color& color);

protected:
  bool onProcessMessage(Message* msg) override;

  void onExit();
  void onCloseWindow();
  void onFgBgColorChange(const app::Color& color);
  void onColorSlidersChange(ColorSlidersChangeEvent& ev);
  void onColorHexEntryChange(const app::Color& color);
  void onColorTypeButtonClick(Event& ev);
  void onAbsoluteButtonClick(Event& ev);
  void onRelativeButtonClick(Event& ev);

private:
  void selectColorType(app::Color::Type type);
  void setPaletteEntry(const app::Color& color);
  void setAbsolutePaletteEntryChannel(ColorSliders::Channel channel, const app::Color& color);
  void setRelativePaletteEntryChannel(ColorSliders::Channel channel, int delta);
  void setNewPalette(Palette* palette, const char* operationName);
  void updateCurrentSpritePalette(const char* operationName);
  void updateColorBar();
  void onPalChange();
  void resetRelativeInfo();

  app::Color::Type m_type;
  Box m_vbox;
  Box m_topBox;
  Box m_bottomBox;
  RadioButton m_rgbButton;
  RadioButton m_hsvButton;
  HexColorEntry m_hexColorEntry;
  Label m_entryLabel;
  RadioButton m_absButton;
  RadioButton m_relButton;
  RgbSliders m_rgbSliders;
  HsvSliders m_hsvSliders;

  // This variable is used to avoid updating the m_hexColorEntry text
  // when the color change is generated from a
  // HexColorEntry::ColorChange signal. In this way we don't override
  // what the user is writting in the text field.
  bool m_disableHexUpdate;

  ui::Timer m_redrawTimer;
  bool m_redrawAll;

  // True if the palette change must be implant in the UndoHistory
  // (e.g. when two or more changes in the palette are made in short
  // time).
  bool m_implantChange;

  // True if the PaletteChange signal is generated by the same
  // PaletteEntryEditor instance.
  bool m_selfPalChange;

  ScopedConnection m_palChangeConn;

  // Palette used for relative changes.
  Palette m_fromPalette;
  std::map<ColorSliders::Channel, int> m_relDeltas;
};

static PaletteEntryEditor* g_window = NULL;

class PaletteEditorCommand : public Command {
public:
  PaletteEditorCommand();
  Command* clone() const override { return new PaletteEditorCommand(*this); }

protected:
  void onLoadParams(const Params& params) override;
  void onExecute(Context* context) override;
  bool onChecked(Context* context) override;

private:
  bool m_open;
  bool m_close;
  bool m_switch;
  bool m_background;
};

PaletteEditorCommand::PaletteEditorCommand()
  : Command("PaletteEditor",
            "Palette Editor",
            CmdRecordableFlag)
{
  m_open = true;
  m_close = false;
  m_switch = false;
  m_background = false;
}

void PaletteEditorCommand::onLoadParams(const Params& params)
{
  std::string target = params.get("target");
  if (target == "foreground") m_background = false;
  else if (target == "background") m_background = true;

  std::string open_str = params.get("open");
  if (open_str == "true") m_open = true;
  else m_open = false;

  std::string close_str = params.get("close");
  if (close_str == "true") m_close = true;
  else m_close = false;

  std::string switch_str = params.get("switch");
  if (switch_str == "true") m_switch = true;
  else m_switch = false;
}

void PaletteEditorCommand::onExecute(Context* context)
{
  // If this is the first time the command is execute...
  if (!g_window) {
    // If the command says "Close the palette editor" and it is not
    // created yet, we just do nothing.
    if (m_close)
      return;

    // If this is "open" or "switch", we have to create the frame.
    g_window = new PaletteEntryEditor();
  }
  // If the frame is already created and it's visible, close it (only in "switch" or "close" modes)
  else if (g_window->isVisible() && (m_switch || m_close)) {
    // Hide the frame
    g_window->closeWindow(NULL);
    return;
  }

  if (m_switch || m_open) {
    if (!g_window->isVisible()) {
      // Default bounds
      g_window->remapWindow();

      int width = MAX(g_window->getBounds().w, ui::display_w()/2);
      g_window->setBounds(Rect(
          ui::display_w() - width - ToolBar::instance()->getBounds().w,
          ui::display_h() - g_window->getBounds().h - StatusBar::instance()->getBounds().h,
          width, g_window->getBounds().h));

      // Load window configuration
      load_window_pos(g_window, "PaletteEditor");
    }

    // Run the frame in background.
    g_window->openWindow();
    ColorBar::instance()->setPaletteEditorButtonState(true);
  }

  // Show the specified target color
  {
    app::Color color =
      (m_background ? context->settings()->getBgColor():
                      context->settings()->getFgColor());

    g_window->setColor(color);
  }
}

bool PaletteEditorCommand::onChecked(Context* context)
{
  if(!g_window)
  {
    return false;
  }
  return g_window->isVisible();
}

//////////////////////////////////////////////////////////////////////
// PaletteEntryEditor implementation
//
// Based on ColorSelector class.

PaletteEntryEditor::PaletteEntryEditor()
  : Window(WithTitleBar, "Palette Editor (F4)")
  , m_type(app::Color::MaskType)
  , m_vbox(JI_VERTICAL)
  , m_topBox(JI_HORIZONTAL)
  , m_bottomBox(JI_HORIZONTAL)
  , m_rgbButton("RGB", 1, kButtonWidget)
  , m_hsvButton("HSB", 1, kButtonWidget)
  , m_entryLabel("")
  , m_absButton("Abs", 2, kButtonWidget)
  , m_relButton("Rel", 2, kButtonWidget)
  , m_disableHexUpdate(false)
  , m_redrawTimer(250, this)
  , m_redrawAll(false)
  , m_implantChange(false)
  , m_selfPalChange(false)
  , m_fromPalette(0, Palette::MaxColors)
{
  m_topBox.setBorder(gfx::Border(0));
  m_topBox.child_spacing = 0;
  m_bottomBox.setBorder(gfx::Border(0));

  setup_mini_look(&m_rgbButton);
  setup_mini_look(&m_hsvButton);
  setup_mini_look(&m_absButton);
  setup_mini_look(&m_relButton);

  // Top box
  m_topBox.addChild(&m_rgbButton);
  m_topBox.addChild(&m_hsvButton);
  m_topBox.addChild(&m_hexColorEntry);
  m_topBox.addChild(&m_entryLabel);
  m_topBox.addChild(new BoxFiller);
  m_topBox.addChild(&m_absButton);
  m_topBox.addChild(&m_relButton);

  // Main vertical box
  m_vbox.addChild(&m_topBox);
  m_vbox.addChild(&m_rgbSliders);
  m_vbox.addChild(&m_hsvSliders);
  m_vbox.addChild(&m_bottomBox);
  addChild(&m_vbox);

  m_rgbButton.Click.connect(&PaletteEntryEditor::onColorTypeButtonClick, this);
  m_hsvButton.Click.connect(&PaletteEntryEditor::onColorTypeButtonClick, this);
  m_absButton.Click.connect(&PaletteEntryEditor::onAbsoluteButtonClick, this);
  m_relButton.Click.connect(&PaletteEntryEditor::onRelativeButtonClick, this);

  m_rgbSliders.ColorChange.connect(&PaletteEntryEditor::onColorSlidersChange, this);
  m_hsvSliders.ColorChange.connect(&PaletteEntryEditor::onColorSlidersChange, this);
  m_hexColorEntry.ColorChange.connect(&PaletteEntryEditor::onColorHexEntryChange, this);

  m_absButton.setSelected(true);
  selectColorType(app::Color::RgbType);

  // We hook fg/bg color changes (by eyedropper mainly) to update the selected entry color
  ColorBar::instance()->FgColorChange.connect(&PaletteEntryEditor::onFgBgColorChange, this);
  ColorBar::instance()->BgColorChange.connect(&PaletteEntryEditor::onFgBgColorChange, this);

  // We hook the Window::Close event to save the frame position before closing it.
  this->Close.connect(Bind<void>(&PaletteEntryEditor::onCloseWindow, this));

  // We hook App::Exit signal to destroy the g_window singleton at exit.
  App::instance()->Exit.connect(&PaletteEntryEditor::onExit, this);

  // Hook for palette change to redraw the palette editor frame
  m_palChangeConn =
    App::instance()->PaletteChange.connect(&PaletteEntryEditor::onPalChange, this);

  initTheme();
}

void PaletteEntryEditor::setColor(const app::Color& color)
{
  m_rgbSliders.setColor(color);
  m_hsvSliders.setColor(color);
  if (!m_disableHexUpdate)
    m_hexColorEntry.setColor(color);

  PaletteView* palette_editor = ColorBar::instance()->getPaletteView();
  PalettePicks entries;
  palette_editor->getSelectedEntries(entries);
  int i, j, i2;

  // Find the first selected entry
  for (i=0; i<(int)entries.size(); ++i)
    if (entries[i])
      break;

  // Find the first unselected entry after i
  for (i2=i+1; i2<(int)entries.size(); ++i2)
    if (!entries[i2])
      break;

  // Find the last selected entry
  for (j=entries.size()-1; j>=0; --j)
    if (entries[j])
      break;

  if (i == j) {
    m_entryLabel.setTextf(" Entry: %d", i);
  }
  else if (j-i+1 == i2-i) {
    m_entryLabel.setTextf(" Range: %d-%d", i, j);
  }
  else if (i == int(entries.size())) {
    m_entryLabel.setText(" No Entry");
  }
  else {
    m_entryLabel.setText(" Multiple Entries");
  }

  m_topBox.layout();
}

bool PaletteEntryEditor::onProcessMessage(Message* msg)
{
  if (msg->type() == kTimerMessage &&
      static_cast<TimerMessage*>(msg)->timer() == &m_redrawTimer) {
    // Redraw all editors
    if (m_redrawAll) {
      m_redrawAll = false;
      m_implantChange = false;
      m_redrawTimer.stop();

      // Call all observers of PaletteChange event.
      m_selfPalChange = true;
      App::instance()->PaletteChange();
      m_selfPalChange = false;

      // Redraw all editors
      try {
        ContextWriter writer(UIContext::instance());
        Document* document(writer.document());
        if (document != NULL)
          document->notifyGeneralUpdate();
      }
      catch (...) {
        // Do nothing
      }
    }
    // Redraw just the current editor
    else {
      m_redrawAll = true;
      if (current_editor != NULL)
        current_editor->updateEditor();
    }
  }
  return Window::onProcessMessage(msg);
}

void PaletteEntryEditor::onExit()
{
  delete this;
}

void PaletteEntryEditor::onCloseWindow()
{
  // Save window configuration
  save_window_pos(this, "PaletteEditor");

  // Uncheck the "Edit Palette" button.
  ColorBar::instance()->setPaletteEditorButtonState(false);
}

void PaletteEntryEditor::onFgBgColorChange(const app::Color& color)
{
  if (color.isValid() && color.getType() == app::Color::IndexType) {
    setColor(color);
    resetRelativeInfo();
  }
}

void PaletteEntryEditor::onColorSlidersChange(ColorSlidersChangeEvent& ev)
{
  setColor(ev.color());

  if (ev.mode() == ColorSliders::Absolute)
    setAbsolutePaletteEntryChannel(ev.channel(), ev.color());
  else
    setRelativePaletteEntryChannel(ev.channel(), ev.delta());

  updateCurrentSpritePalette("Color Change");
  updateColorBar();
}

void PaletteEntryEditor::onColorHexEntryChange(const app::Color& color)
{
  // Disable updating the hex entry so we don't override what the user
  // is writting in the text field.
  m_disableHexUpdate = true;

  setColor(color);
  setPaletteEntry(color);
  updateCurrentSpritePalette("Color Change");
  updateColorBar();

  m_disableHexUpdate = false;
}

void PaletteEntryEditor::onColorTypeButtonClick(Event& ev)
{
  RadioButton* source = static_cast<RadioButton*>(ev.getSource());

  if (source == &m_rgbButton) selectColorType(app::Color::RgbType);
  else if (source == &m_hsvButton) selectColorType(app::Color::HsvType);
}

void PaletteEntryEditor::onAbsoluteButtonClick(Event& ev)
{
  m_rgbSliders.setMode(ColorSliders::Absolute);
  m_hsvSliders.setMode(ColorSliders::Absolute);
}

void PaletteEntryEditor::onRelativeButtonClick(Event& ev)
{
  m_rgbSliders.setMode(ColorSliders::Relative);
  m_hsvSliders.setMode(ColorSliders::Relative);
  resetRelativeInfo();
}

void PaletteEntryEditor::setPaletteEntry(const app::Color& color)
{
  PaletteView* palView = ColorBar::instance()->getPaletteView();
  PalettePicks entries;
  palView->getSelectedEntries(entries);

  color_t new_pal_color = doc::rgba(color.getRed(),
                                    color.getGreen(),
                                    color.getBlue(), 255);

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (entries[c])
      palette->setEntry(c, new_pal_color);
  }
}

void PaletteEntryEditor::setAbsolutePaletteEntryChannel(ColorSliders::Channel channel, const app::Color& color)
{
  PaletteView* palView = ColorBar::instance()->getPaletteView();
  PalettePicks entries;
  palView->getSelectedEntries(entries);

  int begSel, endSel;
  if (!palView->getSelectedRange(begSel, endSel))
    return;

  uint32_t src_color;
  int r, g, b;

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (!entries[c])
      continue;

    // Get the current RGB values of the palette entry
    src_color = palette->getEntry(c);
    r = rgba_getr(src_color);
    g = rgba_getg(src_color);
    b = rgba_getb(src_color);

    switch (m_type) {

      case app::Color::RgbType:
        // Modify one entry
        if (begSel == endSel) {
          r = color.getRed();
          g = color.getGreen();
          b = color.getBlue();
        }
        // Modify one channel a set of entries
        else {
          // Setup the new RGB values depending of the modified channel.
          switch (channel) {
            case ColorSliders::Red:
              r = color.getRed();
            case ColorSliders::Green:
              g = color.getGreen();
              break;
            case ColorSliders::Blue:
              b = color.getBlue();
              break;
          }
        }
        break;

      case app::Color::HsvType:
        {
          Hsv hsv;

          // Modify one entry
          if (begSel == endSel) {
            hsv.hue(color.getHue());
            hsv.saturation(double(color.getSaturation()) / 100.0);
            hsv.value(double(color.getValue()) / 100.0);
          }
          // Modify one channel a set of entries
          else {
            // Convert RGB to HSV
            hsv = Hsv(Rgb(r, g, b));

            // Only modify the desired HSV channel
            switch (channel) {
              case ColorSliders::Hue:
                hsv.hue(color.getHue());
                break;
              case ColorSliders::Saturation:
                hsv.saturation(double(color.getSaturation()) / 100.0);
                break;
              case ColorSliders::Value:
                hsv.value(double(color.getValue()) / 100.0);
                break;
            }
          }

          // Convert HSV back to RGB
          Rgb rgb(hsv);
          r = rgb.red();
          g = rgb.green();
          b = rgb.blue();
        }
        break;
    }

    palette->setEntry(c, doc::rgba(r, g, b, 255));
  }
}

void PaletteEntryEditor::setRelativePaletteEntryChannel(ColorSliders::Channel channel, int delta)
{
  PaletteView* palView = ColorBar::instance()->getPaletteView();
  PalettePicks entries;
  palView->getSelectedEntries(entries);

  // Update modified delta
  m_relDeltas[channel] = delta;

  uint32_t src_color;
  int r, g, b;

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (!entries[c])
      continue;

    // Get the current RGB values of the palette entry
    src_color = m_fromPalette.getEntry(c);
    r = rgba_getr(src_color);
    g = rgba_getg(src_color);
    b = rgba_getb(src_color);

    switch (m_type) {

      case app::Color::RgbType:
        r = MID(0, r+m_relDeltas[ColorSliders::Red], 255);
        g = MID(0, g+m_relDeltas[ColorSliders::Green], 255);
        b = MID(0, b+m_relDeltas[ColorSliders::Blue], 255);
        break;

      case app::Color::HsvType: {
        // Convert RGB to HSV
        Hsv hsv(Rgb(r, g, b));

        double h = hsv.hue()+m_relDeltas[ColorSliders::Hue];
        double s = 100.0*hsv.saturation()+m_relDeltas[ColorSliders::Saturation];
        double v = 100.0*hsv.value()+m_relDeltas[ColorSliders::Value];

        if (h < 0.0) h += 360.0;
        else if (h > 360.0) h -= 360.0;

        hsv.hue       (MID(0.0, h, 360.0));
        hsv.saturation(MID(0.0, s, 100.0) / 100.0);
        hsv.value     (MID(0.0, v, 100.0) / 100.0);

        // Convert HSV back to RGB
        Rgb rgb(hsv);
        r = rgb.red();
        g = rgb.green();
        b = rgb.blue();
        break;
      }

    }

    palette->setEntry(c, doc::rgba(r, g, b, 255));
  }
}

void PaletteEntryEditor::selectColorType(app::Color::Type type)
{
  m_type = type;
  m_rgbSliders.setVisible(type == app::Color::RgbType);
  m_hsvSliders.setVisible(type == app::Color::HsvType);

  resetRelativeInfo();

  switch (type) {
    case app::Color::RgbType: m_rgbButton.setSelected(true); break;
    case app::Color::HsvType: m_hsvButton.setSelected(true); break;
  }

  m_vbox.layout();
  m_vbox.invalidate();
}

void PaletteEntryEditor::updateCurrentSpritePalette(const char* operationName)
{
  if (UIContext::instance()->activeDocument() &&
      UIContext::instance()->activeDocument()->sprite()) {
    try {
      ContextWriter writer(UIContext::instance());
      Document* document(writer.document());
      Sprite* sprite(writer.sprite());
      Palette* newPalette = get_current_palette(); // System current pal
      frame_t frame = writer.frame();
      Palette* currentSpritePalette = sprite->palette(frame); // Sprite current pal
      int from, to;

      // Check differences between current sprite palette and current system palette
      from = to = -1;
      currentSpritePalette->countDiff(newPalette, &from, &to);

      if (from >= 0 && to >= from) {
        DocumentUndo* undo = document->undoHistory();
        Cmd* cmd = new cmd::SetPalette(sprite, frame, newPalette);

        // Add undo information to save the range of pal entries that will be modified.
        if (m_implantChange &&
            undo->lastExecutedCmd() &&
            undo->lastExecutedCmd()->label() == operationName) {
          // Implant the cmd in the last CmdSequence if it's
          // related about color palette modifications
          ASSERT(dynamic_cast<CmdSequence*>(undo->lastExecutedCmd()));
          static_cast<CmdSequence*>(undo->lastExecutedCmd())->add(cmd);
          cmd->execute(UIContext::instance());
        }
        else {
          Transaction transaction(writer.context(), operationName, ModifyDocument);
          transaction.execute(cmd);
          transaction.commit();
        }
      }
    }
    catch (base::Exception& e) {
      Console::showException(e);
    }
  }

  PaletteView* palette_editor = ColorBar::instance()->getPaletteView();
  palette_editor->invalidate();

  if (!m_redrawTimer.isRunning())
    m_redrawTimer.start();

  m_redrawAll = false;
  m_implantChange = true;
}

void PaletteEntryEditor::updateColorBar()
{
  ColorBar::instance()->invalidate();
}

void PaletteEntryEditor::onPalChange()
{
  if (!m_selfPalChange) {
    PaletteView* palette_editor = ColorBar::instance()->getPaletteView();
    int index = palette_editor->getSelectedEntry();
    if (index >= 0)
      setColor(app::Color::fromIndex(index));

    resetRelativeInfo();

    // Redraw the window
    invalidate();
  }
}

void PaletteEntryEditor::resetRelativeInfo()
{
  m_rgbSliders.resetRelativeSliders();
  m_hsvSliders.resetRelativeSliders();
  get_current_palette()->copyColorsTo(&m_fromPalette);
  m_relDeltas.clear();
}

Command* CommandFactory::createPaletteEditorCommand()
{
  return new PaletteEditorCommand;
}

} // namespace app

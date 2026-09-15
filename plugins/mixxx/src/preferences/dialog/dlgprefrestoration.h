/* ========================================
 *  Mixxx restoration - the preferences page
 *
 *  Declick and Dehum are per library rather than per deck, which is the shape
 *  the settings take because of where the restoration runs: the pipeline is
 *  sized when a track is loaded and its output is what fills CachingReader's
 *  decoded chunks, so a parameter that moved mid-track would mean throwing those
 *  chunks away and decoding them again under the deck. Set it here, for the
 *  collection; a change reaches a deck the next time it loads something.
 *
 *  The controls are in the cores' own units - milliseconds, hertz, counts - and
 *  their ranges are exactly the ranges the VST2 and VirtualDJ sliders cover, so
 *  every figure in ../../../../foobar2000_dsp/README.md describes this page too.
 * ======================================== */

#pragma once

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgprefrestorationdlg.h"
#include "preferences/usersettings.h"
#include "restoration/restorationsettings.h"

class QWidget;

class DlgPrefRestoration : public DlgPreferencePage,
                           public Ui::DlgPrefRestorationDlg {
    Q_OBJECT
  public:
    DlgPrefRestoration(QWidget* pParent, UserSettingsPointer pConfig);
    ~DlgPrefRestoration() override = default;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  private slots:
    //! Frequency and Rumble read 0 as "automatic" and "off", and their lowest
    //! legal value above that is 10 Hz - a notch below that overlaps DC. So the
    //! gap between them is not a value either control can hold, and stepping
    //! through it jumps rather than stopping somewhere the core would silently
    //! round away from.
    void slotSnapOffZone(double value);

  private:
    void applyTo(const restoration::Settings& settings);
    restoration::Settings settingsFromControls() const;

    UserSettingsPointer m_pConfig;
};

#include "preferences/dialog/dlgprefrestoration.h"

#include <QtGlobal>

#include "restoration/restorationconfig.h"

namespace {
//! The lowest frequency either off-zone control may actually hold. Below this a
//! notch overlaps DC, which is why dehum::Params::sanitize() refuses it.
constexpr double kLowestLine = 10.0;
} // namespace

DlgPrefRestoration::DlgPrefRestoration(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pConfig(pConfig) {
    setupUi(this);

    connect(dehumFrequency,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &DlgPrefRestoration::slotSnapOffZone);
    connect(dehumRumble,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &DlgPrefRestoration::slotSnapOffZone);

    slotUpdate();
}

void DlgPrefRestoration::slotUpdate() {
    applyTo(restoration::readSettings(m_pConfig));
}

void DlgPrefRestoration::slotResetToDefaults() {
    // The cores' calibrated defaults, and both filters off - see
    // restorationsettings.h for why off is the default rather than on.
    applyTo(restoration::Settings());
}

void DlgPrefRestoration::slotApply() {
    restoration::writeSettings(m_pConfig, settingsFromControls());
}

void DlgPrefRestoration::slotSnapOffZone(double value) {
    auto* pSpin = qobject_cast<QDoubleSpinBox*>(sender());
    if (!pSpin || value <= 0.0 || value >= kLowestLine) {
        return;
    }
    // Landed between "off" and the lowest legal line. Which way the user was
    // going decides where they end up: stepping up from 0 means they wanted a
    // frequency, stepping down from 10 means they wanted it off.
    const QSignalBlocker blocker(pSpin);
    pSpin->setValue(value < kLowestLine * 0.5 ? 0.0 : kLowestLine);
}

void DlgPrefRestoration::applyTo(const restoration::Settings& settings) {
    declickGroup->setChecked(settings.declickEnabled);
    declickSensitivity->setValue(settings.declick.sensitivity);
    declickExtent->setValue(settings.declick.extent);
    declickMaxRepair->setValue(settings.declick.maxLengthMs);
    declickDepth->setValue(settings.declick.depth);
    declickPasses->setValue(settings.declick.passes);
    declickOrder->setValue(settings.declick.order);
    declickDryWet->setValue(settings.declick.dryWet);

    dehumGroup->setChecked(settings.dehumEnabled);
    dehumSensitivity->setValue(settings.dehum.sensitivity);
    dehumBandwidth->setValue(settings.dehum.bandwidth);
    dehumSearchTo->setValue(settings.dehum.searchTo);
    dehumHarmonics->setValue(settings.dehum.harmonics);
    dehumFrequency->setValue(settings.dehum.frequency);
    dehumRumble->setValue(settings.dehum.rumbleHz);
    dehumDryWet->setValue(settings.dehum.dryWet);
}

restoration::Settings DlgPrefRestoration::settingsFromControls() const {
    restoration::Settings s;

    s.declickEnabled = declickGroup->isChecked();
    s.declick.sensitivity = static_cast<float>(declickSensitivity->value());
    s.declick.extent = static_cast<float>(declickExtent->value());
    s.declick.maxLengthMs = static_cast<float>(declickMaxRepair->value());
    s.declick.depth = static_cast<float>(declickDepth->value());
    s.declick.passes = declickPasses->value();
    // Steps of 8, the same seven settings the VST2 and VirtualDJ ports reach.
    // The spin box steps in eights but can be typed into, so it is snapped here
    // rather than trusted.
    s.declick.order = ((declickOrder->value() + 4) / 8) * 8;
    s.declick.dryWet = static_cast<float>(declickDryWet->value());

    s.dehumEnabled = dehumGroup->isChecked();
    s.dehum.sensitivity = static_cast<float>(dehumSensitivity->value());
    s.dehum.bandwidth = static_cast<float>(dehumBandwidth->value());
    s.dehum.searchTo = static_cast<float>(dehumSearchTo->value());
    s.dehum.harmonics = dehumHarmonics->value();
    s.dehum.frequency = static_cast<float>(dehumFrequency->value());
    s.dehum.rumbleHz = static_cast<float>(dehumRumble->value());
    s.dehum.dryWet = static_cast<float>(dehumDryWet->value());

    s.sanitize();
    return s;
}

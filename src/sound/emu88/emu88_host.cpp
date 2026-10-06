/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          88emu glue, see emu88_host.h. Device facts (which panel, which
 *          switches, which artwork) follow 88emuPlayer's editor
 *          (Emu88EditorBindings.h, Emu88Editor.cpp) so a board looks and
 *          answers here as it does there.
 */
#include "emu88_host.h"

#include "88lib/deviceModel.h"
#include "88lib/hardwareDevice.h"
#include "88lib/boards/laBoard.h"
#include "88lib/boards/sc88pro.h"
#include "88lib/boards/sc88types.h"
#include "88lib/rom/romloader.h"

#include "baseLib/filesystem.h"
#include "synthLib/audioTypes.h"
#include "synthLib/midiBufferParser.h"
#include "synthLib/midiTypes.h"
#include "synthLib/resampler.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using emu88Lib::DeviceModel;

namespace
{
    // 88emu's CLI ids, in DeviceModel order (Emu88LaunchOptions.cpp).
    constexpr const char *g_keys[] = { "sc88", "sc88vl", "sc88pro", "sc8850", "sc55mk2", "sc55",
                                       "sc55st", "cm300", "scb55", "rlp3237", "sc155", "sc155mk2",
                                       "xpgs", "sc8820", "cm32p", "vegspro", "scc1a", "cm64",
                                       "cm32l", "nu10b", "miig5", "mt32old", "mt32new", "cm32ln" };
    static_assert(std::size(g_keys) == emu88Lib::g_deviceMenuOrder.size());

    bool toModel(const int model, DeviceModel &out)
    {
        if (model < 0 || !emu88Lib::isDeviceModelValue(static_cast<uint32_t>(model)))
            return false;
        out = static_cast<DeviceModel>(model);
        return true;
    }

    constexpr uint32_t proBit(const emu88Lib::Sc88ProButton b) { return 1u << static_cast<uint8_t>(b); }

    // Emu88EditorBindings.h: the SC-55 family populates only these matrix positions.
    constexpr uint32_t g_sc55Buttons = emu88Lib::buttonBit(emu88Lib::Button::Power) | emu88Lib::buttonBit(emu88Lib::Button::InstL) | emu88Lib::buttonBit(emu88Lib::Button::InstR) | emu88Lib::buttonBit(emu88Lib::Button::InstMute) | emu88Lib::buttonBit(emu88Lib::Button::InstAll) | emu88Lib::buttonBit(emu88Lib::Button::MidiChL) | emu88Lib::buttonBit(emu88Lib::Button::MidiChR) | emu88Lib::buttonBit(emu88Lib::Button::ChorusL) | emu88Lib::buttonBit(emu88Lib::Button::ChorusR) | emu88Lib::buttonBit(emu88Lib::Button::PanL) | emu88Lib::buttonBit(emu88Lib::Button::PanR) | emu88Lib::buttonBit(emu88Lib::Button::PartL) | emu88Lib::buttonBit(emu88Lib::Button::PartR) | emu88Lib::buttonBit(emu88Lib::Button::KeyShiftL) | emu88Lib::buttonBit(emu88Lib::Button::KeyShiftR) | emu88Lib::buttonBit(emu88Lib::Button::ReverbL) | emu88Lib::buttonBit(emu88Lib::Button::ReverbR) | emu88Lib::buttonBit(emu88Lib::Button::LevelL) | emu88Lib::buttonBit(emu88Lib::Button::LevelR);
    constexpr uint32_t g_mt32Buttons = 0x1f1f;

    bool usesLaBoardButtons(const DeviceModel m) { return emu88Lib::isLaModel(m) || m == DeviceModel::Cm64; }

    uint32_t buttonMask(const DeviceModel m)
    {
        if (usesLaBoardButtons(m))
            return g_mt32Buttons;
        if (m == DeviceModel::Xpgs || m == DeviceModel::VeGsPro || m == DeviceModel::Sc8820 || emu88Lib::isCmModel(m) || emu88Lib::isGmModuleModel(m))
            return 0;
        if (m == DeviceModel::Sc88Pro)
            return ~proBit(emu88Lib::Sc88ProButton::Power);
        if (m == DeviceModel::Sc88VL)
            return ~proBit(emu88Lib::Sc88ProButton::Preview);
        if (emu88Lib::isSc55Model(m))
            return emu88Lib::getSc55DeviceProfile(m).panel == emu88Lib::Sc55Panel::None ? 0 : g_sc55Buttons;
        return ~0u;
    }

    std::mutex g_romMutex;

    void fill(char *dst, const size_t size, const std::string &src)
    {
        if (!size)
            return;
        const auto n = std::min(src.size(), size - 1);
        std::memcpy(dst, src.data(), n);
        dst[n] = 0;
    }

    struct RomRow {
        std::string label, names, file;
        size_t      size   = 0;
        int         status = EMU88H_ROM_MISSING;
    };

    // RomInventory::describeRequirements(), as rows.
    void describe(const emu88Lib::RomInventory &inv, const emu88Lib::RomDevice dev, const std::string &prefix, std::vector<RomRow> &rows)
    {
        using namespace emu88Lib;
        std::set<std::pair<RomSlot, uint8_t>> slots;
        for (const auto &spec : g_romFileSpecs)
            if (spec.device == dev)
                slots.emplace(spec.slot, spec.index);
        for (const auto &entry : g_romRegistry)
            if (usedBy(entry, dev))
                slots.emplace(entry.slot, entry.index);

        for (const auto &[slot, index] : slots) {
            RomRow row;
            row.label = prefix + toString(slot);
            if (std::count_if(slots.begin(), slots.end(), [s = slot](const auto &o) { return o.first == s; }) > 1)
                row.label += ' ' + std::to_string(index + 1);

            for (const auto &spec : g_romFileSpecs)
                if (spec.device == dev && spec.slot == slot && spec.index == index) {
                    if (!row.names.empty())
                        row.names += " / ";
                    row.names += spec.filename;
                    if (!row.size)
                        row.size = spec.size;
                }
            if (!row.size)
                for (const auto &entry : g_romRegistry)
                    if (usedBy(entry, dev) && entry.slot == slot && entry.index == index) {
                        row.size = entry.size;
                        break;
                    }

            const auto *found = inv.find(dev, slot, index);
            if (found) {
                row.file   = baseLib::filesystem::getFilenameWithoutPath(found->path);
                row.size   = found->size();
                row.status = (found->entry && !found->hashMismatch) ? EMU88H_ROM_OK : EMU88H_ROM_UNVERIFIED;
            } else if (inv.has(dev, slot, index))
                row.status = EMU88H_ROM_DERIVED;
            else if (isCompositeChip(dev, slot, index))
                row.status = EMU88H_ROM_ALTERNATIVE;
            else
                row.status = EMU88H_ROM_MISSING;
            rows.push_back(std::move(row));
        }
    }
}

/* == Catalogue ========================================================= */

extern "C" int
emu88h_model_count(void)
{
    return static_cast<int>(emu88Lib::g_deviceMenuOrder.size());
}

extern "C" int
emu88h_model_at(const int index)
{
    if (index < 0 || static_cast<size_t>(index) >= emu88Lib::g_deviceMenuOrder.size())
        return -1;
    return static_cast<int>(emu88Lib::g_deviceMenuOrder[index]);
}

extern "C" const char *
emu88h_model_name(const int model)
{
    DeviceModel m;
    return toModel(model, m) ? emu88Lib::getDeviceProfile(m).displayName : nullptr;
}

extern "C" const char *
emu88h_model_key(const int model)
{
    DeviceModel m;
    return toModel(model, m) ? g_keys[model] : nullptr;
}

extern "C" int
emu88h_model_from_key(const char *key)
{
    if (!key)
        return -1;
    for (size_t i = 0; i < std::size(g_keys); ++i)
        if (!strcmp(key, g_keys[i]))
            return static_cast<int>(i);
    return -1;
}

extern "C" int
emu88h_model_panel(const int model)
{
    DeviceModel m;
    if (!toModel(model, m))
        return EMU88H_PANEL_NONE;
    if (m == DeviceModel::Sc8850)
        return EMU88H_PANEL_SC8850;
    if (emu88Lib::isCmModel(m) || emu88Lib::isLaModel(m))
        return EMU88H_PANEL_CM;
    return buttonMask(m) ? EMU88H_PANEL_SC88 : EMU88H_PANEL_NONE;
}

extern "C" int
emu88h_model_has_lcd(const int model)
{
    DeviceModel m;
    return toModel(model, m) && emu88Lib::deviceHasLcd(m);
}

extern "C" int
emu88h_model_has_second_lcd(const int model)
{
    DeviceModel m;
    return toModel(model, m) && emu88Lib::deviceHasSecondLcd(m);
}

extern "C" int
emu88h_model_power_standby(const int model)
{
    DeviceModel m;
    return toModel(model, m) && emu88Lib::getPowerSwitch(m) == emu88Lib::PowerSwitch::Standby;
}

extern "C" int
emu88h_model_has_knob(const int model)
{
    DeviceModel m;
    return toModel(model, m) && emu88Lib::hasPanelKnob(m);
}

extern "C" uint32_t
emu88h_model_button_mask(const int model)
{
    DeviceModel m;
    return toModel(model, m) ? buttonMask(m) : 0;
}

extern "C" const char *
emu88h_model_artwork(const int model)
{
    DeviceModel m;
    if (!toModel(model, m))
        return "sc88exp";
    switch (m) {
        case DeviceModel::Sc88:     return "sc88";
        case DeviceModel::Sc88VL:   return "sc88vl";
        case DeviceModel::Sc88Pro:  return "sc88pro";
        case DeviceModel::Sc8850:   return "sc8850";
        case DeviceModel::Sc55Mk2:
        case DeviceModel::Sc155Mk2: return "sc55mk2";
        case DeviceModel::Sc55Mk1:
        case DeviceModel::Sc155:    return "sc55";
        case DeviceModel::Cm32p:    return "cm32p";
        case DeviceModel::Cm32l:
        case DeviceModel::Cm32ln:
        case DeviceModel::Mt32Old:
        case DeviceModel::Mt32New:  return "cm32l";
        case DeviceModel::Cm64:     return "cm64";
        case DeviceModel::Sc8820:   return "sc8820";
        case DeviceModel::Sc55St:
        case DeviceModel::Cm300:
        case DeviceModel::Scc1a:
        case DeviceModel::Scb55:
        case DeviceModel::Rlp3237:  return "sc55pc";
        default:                    return "sc88exp";
    }
}

/* == ROMs ============================================================== */

extern "C" void
emu88h_set_rom_dirs(const char *const *dirs, const int count)
{
    std::lock_guard lock(g_romMutex);
    bool first = true;
    for (int i = 0; i < count; ++i) {
        if (!dirs[i] || !*dirs[i])
            continue;
        if (first)
            synthLib::RomLoader::setSearchPath(dirs[i]);
        else
            synthLib::RomLoader::addSearchPath(dirs[i], true);
        first = false;
    }
    emu88Lib::RomLoader::rescan();
}

extern "C" void
emu88h_rescan(void)
{
    std::lock_guard lock(g_romMutex);
    emu88Lib::RomLoader::rescan();
}

extern "C" int
emu88h_model_available(const int model)
{
    DeviceModel m;
    if (!toModel(model, m))
        return 0;
    std::lock_guard lock(g_romMutex);
    return emu88Lib::RomLoader::isDeviceAvailable(m) ? 1 : 0;
}

extern "C" int
emu88h_model_roms(const int model, emu88h_rom_t *out, const int max)
{
    DeviceModel m;
    if (!toModel(model, m))
        return 0;
    std::vector<RomRow> rows;
    {
        std::lock_guard lock(g_romMutex);
        const auto inv = emu88Lib::RomLoader::scan();
        const auto dev = emu88Lib::RomLoader::toRomDevice(m);
        if (dev == emu88Lib::RomDevice::Cm64) {
            describe(inv, emu88Lib::RomDevice::Cm32l, "CM-32L: ", rows);
            describe(inv, emu88Lib::RomDevice::Cm32p, "CM-32P: ", rows);
        } else
            describe(inv, dev, "", rows);
    }
    for (int i = 0; i < max && i < static_cast<int>(rows.size()); ++i) {
        fill(out[i].label, sizeof(out[i].label), rows[i].label);
        fill(out[i].names, sizeof(out[i].names), rows[i].names);
        fill(out[i].file, sizeof(out[i].file), rows[i].file);
        out[i].size   = static_cast<uint32_t>(rows[i].size);
        out[i].status = rows[i].status;
    }
    return static_cast<int>(rows.size());
}

extern "C" size_t
emu88h_model_note(const int model, char *buf, const size_t size)
{
    std::string text;
    DeviceModel m;
    if (toModel(model, m)) {
        std::lock_guard lock(g_romMutex);
        const auto inv = emu88Lib::RomLoader::scan();
        const auto dev = emu88Lib::RomLoader::toRomDevice(m);
        if (!inv.isComplete(dev) && inv.missing(dev).empty() && inv.missingFiles(dev).empty())
            text = "The firmware ROMs found belong to different revisions; they have to be a matching pair.";
    }
    if (buf && size)
        fill(buf, size, text);
    return text.size();
}

/* == A board ============================================================ */

struct emu88h {
    DeviceModel           model;
    emu88Lib::BootOptions boot;
    std::atomic<int>      refs { 1 };

    std::mutex                                 lock;
    std::unique_ptr<emu88Lib::HardwareDevice>  device;
    std::atomic<int>                           state { EMU88H_STATE_BOOTING };
    bool                                       bootPending = true;
    uint32_t                                   bootButtons = 0;
    std::atomic<int>                           gain { 100 };
    uint32_t                                   buttons     = 0;
    uint64_t                                   generation  = 1; /* bumped per boot, for the LCD revision */
    uint64_t                                   powerTicket = 0; /* bumped per power switch, voids a boot under way */

    std::array<synthLib::MidiBufferParser, 4> parsers {
        synthLib::MidiBufferParser { synthLib::MidiEventSource::Host }, synthLib::MidiBufferParser { synthLib::MidiEventSource::Host },
        synthLib::MidiBufferParser { synthLib::MidiEventSource::Host }, synthLib::MidiBufferParser { synthLib::MidiEventSource::Host }
    };
    unsigned                             portCount = 1;
    std::vector<synthLib::SMidiEvent>    parsed;
    std::vector<synthLib::SMidiEvent>    midiIn;
    std::vector<synthLib::SMidiEvent>    midiOut;
    std::unique_ptr<synthLib::Resampler> resampler;
    std::vector<float>                   left, right;

    void renderDevice(const synthLib::TAudioOutputs &outputs, const size_t frames)
    {
        const synthLib::TAudioInputs inputs {};
        device->process(inputs, outputs, frames, midiIn, midiOut);
        midiIn.clear();
        midiOut.clear(); /* nothing listens to the board's MIDI OUT */
    }
};

extern "C" emu88h_t *
emu88h_create(const int model, const int factory_reset, const int fast_boot)
{
    DeviceModel m;
    if (!toModel(model, m))
        return nullptr;
    auto *h               = new emu88h;
    h->model              = m;
    h->boot.factoryReset  = factory_reset != 0;
    h->boot.fastBoot      = fast_boot != 0;
    h->portCount          = std::clamp<unsigned>(emu88Lib::getDeviceProfile(m).groupCount, 1, 4);
    return h;
}

extern "C" void
emu88h_retain(emu88h_t *h)
{
    if (h)
        h->refs.fetch_add(1);
}

extern "C" void
emu88h_release(emu88h_t *h)
{
    if (h && h->refs.fetch_sub(1) == 1)
        delete h;
}

extern "C" int
emu88h_model(const emu88h_t *h)
{
    return h ? static_cast<int>(h->model) : -1;
}

static void
boot(emu88h_t *h)
{
    emu88Lib::BootOptions options;
    uint64_t              ticket;
    {
        std::lock_guard lock(h->lock);
        ticket                      = h->powerTicket;
        h->bootPending              = false;
        options                     = h->boot;
        options.initialPanelButtons = h->bootButtons & buttonMask(h->model);
        h->state                    = EMU88H_STATE_BOOTING;
    }

    std::unique_ptr<emu88Lib::HardwareDevice> device;
    {
        std::lock_guard romLock(g_romMutex);
        if (emu88Lib::RomLoader::isDeviceAvailable(h->model)) {
            synthLib::DeviceCreateParams params;
            params.customData = static_cast<uint32_t>(h->model);
            device            = std::make_unique<emu88Lib::HardwareDevice>(params, options);
            if (!device->isValid())
                device.reset();
        }
    }

    std::lock_guard lock(h->lock);
    if (h->powerTicket != ticket) /* switched off (and maybe on again) meanwhile */
        return;
    h->device = std::move(device);
    h->resampler.reset();
    h->midiIn.clear();
    ++h->generation;
    if (h->device) {
        h->device->setPanelButtons(h->buttons & buttonMask(h->model));
        h->state = EMU88H_STATE_ON;
    } else
        h->state = EMU88H_STATE_FAILED;
}

extern "C" void
emu88h_render(emu88h_t *h, float *out, const uint32_t frames, const uint32_t out_rate)
{
    std::fill_n(out, static_cast<size_t>(frames) * 2, 0.0f);
    if (!h)
        return;

    bool pending;
    {
        std::lock_guard lock(h->lock);
        pending = h->bootPending;
    }
    if (pending)
        boot(h);

    std::lock_guard lock(h->lock);
    if (!h->device)
        return;

    const auto in = h->device->getSamplerate();
    if (std::abs(in - static_cast<float>(out_rate)) < 0.5f)
        h->resampler.reset();
    else if (!h->resampler || h->resampler->getSamplerateIn() != in || h->resampler->getSamplerateOut() != static_cast<float>(out_rate))
        h->resampler = std::make_unique<synthLib::Resampler>(in, static_cast<float>(out_rate), synthLib::Resampler::Mode::MameHq);

    h->left.resize(frames);
    h->right.resize(frames);
    synthLib::TAudioOutputs outputs {};
    outputs[0] = h->left.data();
    outputs[1] = h->right.data();
    if (h->resampler)
        h->resampler->process(outputs, 2, frames, false, [h](synthLib::TAudioOutputs &o, const uint32_t count) { h->renderDevice(o, count); });
    else
        h->renderDevice(outputs, frames);

    const float g = static_cast<float>(h->gain.load()) / 100.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        out[i * 2]     = h->left[i] * g;
        out[i * 2 + 1] = h->right[i] * g;
    }
}

extern "C" void
emu88h_midi(emu88h_t *h, const unsigned port, const uint8_t *data, const size_t len)
{
    if (!h || !data)
        return;
    std::lock_guard lock(h->lock);
    /* A board that is off or still booting does not hear it, as the hardware would not. */
    if (!h->device)
        return;
    const auto p      = port % h->portCount;
    auto      &parser = h->parsers[p];
    for (size_t i = 0; i < len; ++i)
        parser.write(data[i]);
    h->parsed.clear();
    parser.getEvents(h->parsed);
    for (auto &e : h->parsed) {
        e.port   = static_cast<uint8_t>(p);
        e.offset = 0;
        h->midiIn.push_back(std::move(e));
    }
}

extern "C" void
emu88h_set_gain(emu88h_t *h, const int percent)
{
    if (h)
        h->gain = std::clamp(percent, 0, 200);
}

extern "C" int
emu88h_gain(emu88h_t *h)
{
    return h ? h->gain.load() : 100;
}

extern "C" void
emu88h_set_buttons(emu88h_t *h, const uint32_t buttons)
{
    if (!h)
        return;
    std::lock_guard lock(h->lock);
    h->buttons = buttons;
    if (h->device)
        h->device->setPanelButtons(buttons & buttonMask(h->model));
}

extern "C" void
emu88h_turn_encoder(emu88h_t *h, const int detents)
{
    if (!h)
        return;
    std::lock_guard lock(h->lock);
    if (h->device)
        h->device->turnPanelEncoder(detents);
}

extern "C" int
emu88h_state(emu88h_t *h)
{
    return h ? h->state.load() : EMU88H_STATE_OFF;
}

extern "C" void
emu88h_set_power(emu88h_t *h, const int on, const uint32_t held_buttons)
{
    if (!h)
        return;
    std::unique_ptr<emu88Lib::HardwareDevice> old;
    {
        std::lock_guard lock(h->lock);
        if (on) {
            if (h->device || h->bootPending || h->state == EMU88H_STATE_BOOTING)
                return;
            ++h->powerTicket;
            h->bootPending = true;
            h->bootButtons = held_buttons;
            h->state       = EMU88H_STATE_BOOTING;
        } else {
            ++h->powerTicket;
            h->bootPending = false;
            old            = std::move(h->device);
            h->resampler.reset();
            h->midiIn.clear();
            ++h->generation;
            h->state = EMU88H_STATE_OFF;
        }
    }
}

extern "C" uint32_t
emu88h_leds(emu88h_t *h)
{
    if (!h)
        return 0;
    std::lock_guard lock(h->lock);
    return h->device ? h->device->displaySnapshot().leds : 0;
}

/* emu88_lcd.cpp */
extern void emu88_lcd_draw(DeviceModel model, unsigned screen, const emu88Lib::HardwareDevice::DisplaySnapshot::Screen *snapshot, uint32_t *argb);

extern "C" int
emu88h_lcd_render(emu88h_t *h, const unsigned screen, uint32_t *argb, uint64_t *revision, int *powered)
{
    if (!h || screen > 1)
        return 0;
    emu88Lib::HardwareDevice::DisplaySnapshot snapshot;
    uint64_t                                  generation;
    bool                                      on;
    {
        std::lock_guard lock(h->lock);
        on         = h->device != nullptr;
        generation = h->generation;
        if (on)
            snapshot = h->device->displaySnapshot();
    }
    /* The board's own revision counts its display writes; ours tells boards apart. */
    const uint64_t rev = (generation << 40) ^ snapshot.revision ^ (on ? 0 : 1);
    if (revision && *revision == rev)
        return 0;
    if (revision)
        *revision = rev;
    if (powered)
        *powered = on && snapshot.screens[screen].powered;
    emu88_lcd_draw(h->model, screen, on ? &snapshot.screens[screen] : nullptr, argb);
    return 1;
}

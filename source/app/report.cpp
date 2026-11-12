#include "app.hpp"

#include "feedback.hpp"
#include "platform.hpp"
#include "qr_code.hpp"
#include "storage.hpp"
#include "ui.hpp"
#include "updater.hpp"

#include <string>

// Settings > Report Issue: where to report a bug, join the Discord server or
// reach the developer. Each place is a QR code on the top screen, since a report
// is much easier to type on a phone than on the console.

namespace {

struct Place {
    std::string label;
    ui::Icon icon;
    u32 iconColor;
    std::string shown; // the address under the code, without "https://"
    QrCode code;
};

} // namespace

void App::reportScreen()
{
    feedback::Console console;
    console.version = APP_VERSION;
    console.cia = updater::runningAsCia();
    console.model = feedback::modelName(platform::systemModel());
    console.system = platform::systemVersion();
    const std::string details = feedback::summary(console);

    // The bug report link carries the details, so it is long and gets the
    // compact code. Should it ever not fit, the plain form still works.
    QrCode issue(feedback::newIssueLink(console), QrCode::MaxVersion, true);
    if (!issue.ok()) {
        issue = QrCode(std::string(feedback::IssuesPage) + "/new");
    }
    const Place places[] = {
        {"Report a Bug", ui::Icon::Warning, ui::color::IconOrange,
         feedback::withoutScheme(feedback::IssuesPage), issue},
        {"Discord Server", ui::Icon::Chip, ui::color::IconPink,
         feedback::withoutScheme(feedback::DiscordInvite),
         QrCode(feedback::DiscordInvite)},
        {"Developer on X", ui::Icon::Globe, ui::color::IconTeal,
         feedback::withoutScheme(feedback::DeveloperPage),
         QrCode(feedback::DeveloperPage)},
    };
    constexpr int PlaceCount = sizeof(places) / sizeof(places[0]);

    int focus = 0;
    int shown = 0; // stays on the last place while the ring is on Back
    while (ui::keepRunning() && !quit_) {
        ui::ScreenSpec spec;
        spec.back = "Back";
        for (int index = 0; index < PlaceCount; ++index) {
            ui::Control control;
            control.rect = {30, 12 + index * 44.0f, 260, 36};
            control.label = places[index].label;
            control.icon = places[index].icon;
            control.iconColor = places[index].iconColor;
            control.scale = ui::type::Small + 0.06f;
            spec.controls.push_back(control);
        }
        spec.drawTop = [&]() {
            if (focus >= 0 && focus < PlaceCount) {
                shown = focus;
            }
            const Place &place = places[shown];
            ui::drawTopChrome("Report an Issue", ui::Icon::Warning,
                              ui::color::IconOrange);
            ui::drawPanel();
            // The code is centred in a column wide enough for the largest
            // one (45 modules of 3 px and its quiet zone), so the words to
            // its right stay put when a smaller code shows.
            const float column = 45 * 3 + 12;
            const float px = place.code.size() <= 25 ? 4 : 3;
            const float side = place.code.size() * px;
            const float codeX = ui::TopPanel.x + 12 + (column - side) / 2;
            const float codeY = ui::TopPanel.y + (ui::TopPanel.h - side) / 2;
            place.code.draw(codeX, codeY, px);

            // Name and address as one block, centred beside the code like
            // the code itself.
            const float left = ui::TopPanel.x + 12 + column + 8;
            const float width = ui::TopPanel.x + ui::TopPanel.w - 10 - left;
            const float block = ui::lineHeight(ui::type::Small) + 6 +
                                ui::lineHeight(ui::type::Tiny);
            const float y = ui::TopPanel.y + (ui::TopPanel.h - block) / 2;
            ui::drawMarquee(place.label, left, y, ui::type::Small,
                            ui::color::InkStrong, width);
            ui::drawMarquee(place.shown, left,
                            y + ui::lineHeight(ui::type::Small) + 6,
                            ui::type::Tiny, ui::color::Accent, width);
        };
        // The details to mention and where log.txt and crash.txt are, for
        // the report to attach (they are too long for the code), midway
        // between the buttons and Back.
        spec.drawBottom = [&]() {
            const ui::Rect panel = {13, 143, 294, 58};
            ui::drawPanel(panel);
            ui::drawFacts({{"Details", details}, {"Logs", storage::DataFolder}},
                          panel);
        };

        const ui::ScreenResult result = ui::runScreen(spec, focus);
        if (result.choice < 0 || result.choice >= PlaceCount) {
            return;
        }
        // A tap or A shows that place's code, which focus already does for
        // the D-pad, so there is nothing more to open.
        shown = result.choice;
    }
}

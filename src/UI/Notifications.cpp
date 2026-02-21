// Viewport-overlay notification system.
//
// Notifications appear in the top-right corner of the 3-D viewport (not inside
// any ImGui panel). Each card is:
//   • Dismissable via an ✕ button
//   • Colour-coded by severity (Info / Warning / Critical)
//   • Timestamped with in-game time
//   • Shown newest-first (stack grows downward)
//
// Call SimUI::tickNotifications() once per frame from SimUI::draw() BEFORE
// rendering the notification list.  Call SimUI::pushNotification() from
// anywhere that has access to the SimUI object.

#include "UI/SimUI.hpp"
#include "imgui.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>

// ── Constants ──────────────────────────────────────────────────────────────────
static constexpr float NOTIF_WIDTH       = 320.f;   // card width in pixels
static constexpr float NOTIF_PADDING     = 10.f;    // margin from viewport edge
static constexpr float NOTIF_AUTO_DISMISS= 30.f;    // seconds before auto-dismiss (0 = never)
static constexpr int   NOTIF_MAX_VISIBLE = 6;       // at most this many cards on screen

// ── Severity colours ──────────────────────────────────────────────────────────
static ImVec4 severityBg(NotifSeverity s) {
    switch (s) {
        case NotifSeverity::Info:     return {0.12f, 0.18f, 0.30f, 0.92f};
        case NotifSeverity::Warning:  return {0.30f, 0.22f, 0.05f, 0.92f};
        case NotifSeverity::Critical: return {0.35f, 0.05f, 0.05f, 0.94f};
    }
    return {0.1f, 0.1f, 0.1f, 0.9f};
}

static ImVec4 severityAccent(NotifSeverity s) {
    switch (s) {
        case NotifSeverity::Info:     return {0.40f, 0.65f, 1.00f, 1.f};
        case NotifSeverity::Warning:  return {1.00f, 0.72f, 0.20f, 1.f};
        case NotifSeverity::Critical: return {1.00f, 0.28f, 0.28f, 1.f};
    }
    return {1.f, 1.f, 1.f, 1.f};
}

// ── pushNotification ──────────────────────────────────────────────────────────
void SimUI::pushNotification(const std::string& title,
                             const std::string& message,
                             NotifSeverity       severity,
                             float               gameTime)
{
    // Deduplicate: don't stack the same title if the most recent card matches
    if (!notifications.empty() && notifications.front().title == title)
        return;

    Notification n;
    n.title    = title;
    n.message  = message;
    n.severity = severity;
    n.gameTime = gameTime;
    n.age      = 0.f;
    n.dismissed= false;

    // Insert at front so newest is at top
    notifications.insert(notifications.begin(), n);

    // Cap total history
    while ((int)notifications.size() > 50)
        notifications.pop_back();
}

// ── tickNotifications ─────────────────────────────────────────────────────────
// Age existing cards; fire built-in game-event checks.
void SimUI::tickNotifications(float dt, const World& world) {
    for (auto& n : notifications) n.age += dt;

    // ── Built-in trigger: low population ──────────────────────────────────────
    {
        int pop = 0;
        for (const auto& c : world.creatures) if (c.alive) pop++;

        bool nowLow = 0 < pop && pop < 100;

        if (nowLow && !lowPopNotifFired) {
            pushNotification(
                "Low Population",
                std::string("Only ") + std::to_string(pop) +
                " creatures remain! The ecosystem is at risk of collapse.",
                NotifSeverity::Critical,
                world.simTime);
            lowPopNotifFired = true;
        }
        // Reset the flag once population recovers above 120 (hysteresis)
        if (pop >= 120) lowPopNotifFired = false;
    }
}

// ── drawNotifications ─────────────────────────────────────────────────────────
// Renders notification cards overlaid on the viewport, top-right corner.
// Scrollable list via a child window when more than NOTIF_MAX_VISIBLE cards exist.
void SimUI::drawNotifications() {
    // Remove auto-dismissed cards
    if (NOTIF_AUTO_DISMISS > 0.f) {
        notifications.erase(
            std::remove_if(notifications.begin(), notifications.end(),
                [](const Notification& n){
                    return n.dismissed || n.age > NOTIF_AUTO_DISMISS;
                }),
            notifications.end());
    } else {
        notifications.erase(
            std::remove_if(notifications.begin(), notifications.end(),
                [](const Notification& n){ return n.dismissed; }),
            notifications.end());
    }

    if (notifications.empty()) return;

    // Position: top-right of the actual OS window (not an ImGui panel)
    ImGuiIO& io = ImGui::GetIO();
    float  winW = io.DisplaySize.x;

    float  panelX = winW - NOTIF_WIDTH - NOTIF_PADDING;
    float  panelY = 30.f + NOTIF_PADDING;   // below the menu bar (≈30 px)

    // Max height: 6 cards × ~80 px, with scrollbar if more
    float maxH = NOTIF_MAX_VISIBLE * 90.f;

    ImGui::SetNextWindowPos ({panelX, panelY}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({NOTIF_WIDTH, 0.f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);       // transparent container
    ImGui::SetNextWindowSizeConstraints({NOTIF_WIDTH, 0}, {NOTIF_WIDTH, maxH});

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration   |
        ImGuiWindowFlags_NoInputs       |   // pass-through for 3-D viewport clicks
        ImGuiWindowFlags_NoNav          |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_AlwaysAutoResize;

    // We need mouse input for the dismiss button, so drop NoInputs here
    flags &= ~ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0, 4));

    ImGui::Begin("##Notifications", nullptr, flags);

    // Scrollable child when list is long
    float childH = std::min(maxH, (float)notifications.size() * 90.f);
    ImGui::BeginChild("##NotifScroll", ImVec2(NOTIF_WIDTH, childH),
                      false, ImGuiWindowFlags_None);

    for (int i = 0; i < (int)notifications.size(); i++) {
        Notification& n = notifications[i];

        ImVec4 bg     = severityBg(n.severity);
        ImVec4 accent = severityAccent(n.severity);

        // ── Card background ───────────────────────────────────────────────────
        ImVec2 cardPos  = ImGui::GetCursorScreenPos();
        float  cardH    = 78.f;
        ImVec2 cardBR   = {cardPos.x + NOTIF_WIDTH, cardPos.y + cardH};

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(cardPos, cardBR,
                          IM_COL32((int)(bg.x*255), (int)(bg.y*255),
                                   (int)(bg.z*255), (int)(bg.w*255)),
                          6.f);

        // Accent left border (4 px wide)
        dl->AddRectFilled(
            cardPos,
            {cardPos.x + 4.f, cardPos.y + cardH},
            IM_COL32((int)(accent.x*255), (int)(accent.y*255),
                     (int)(accent.z*255), 255),
            0.f);

        // ── Inner content ─────────────────────────────────────────────────────
        ImGui::SetCursorScreenPos({cardPos.x + 10.f, cardPos.y + 8.f});

        // Message body (wrapped)
        ImGui::SetCursorScreenPos({cardPos.x + 14.f, cardPos.y + 28.f});
        ImGui::PushTextWrapPos(cardPos.x + NOTIF_WIDTH - 36.f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.82f, 0.82f, 1.f));
        ImGui::TextUnformatted(n.message.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        // Timestamp bottom-left
        char timeBuf[32];
        float t = n.gameTime;
        int   day = (int)(t / World::DAY_DURATION) + 1;
        float tod = std::fmod(t, World::DAY_DURATION) / World::DAY_DURATION;
        int   hh  = (int)(tod * 24);
        int   mm  = (int)(std::fmod(tod * 24.f, 1.f) * 60);
        std::snprintf(timeBuf, sizeof(timeBuf), "Day %d  %02d:%02d", day, hh, mm);
        ImGui::SetCursorScreenPos({cardPos.x + 14.f, cardPos.y + cardH - 18.f});
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.50f, 1.f));
        ImGui::TextUnformatted(timeBuf);
        ImGui::PopStyleColor();

        // ── Dismiss button (top-right of card) ───────────────────────────────
        ImGui::SetCursorScreenPos({cardPos.x + NOTIF_WIDTH - 26.f, cardPos.y + 6.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.22f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.6f,0.6f,0.6f,1.f));

        char dismissId[32];
        std::snprintf(dismissId, sizeof(dismissId), "✕##notif%d", i);
        if (ImGui::SmallButton(dismissId))
            n.dismissed = true;

        ImGui::PopStyleColor(4);

        // ── Auto-dismiss fade: dim card as it approaches expiry ───────────────
        if (NOTIF_AUTO_DISMISS > 0.f) {
            float remaining = NOTIF_AUTO_DISMISS - n.age;
            if (remaining < 5.f) {
                float alpha = remaining / 5.f;
                // Overlay a darkening rect to simulate fade
                dl->AddRectFilled(cardPos, cardBR,
                    IM_COL32(0, 0, 0, (int)((1.f - alpha) * 180)),
                    6.f);
            }
        }

        // Advance cursor past the card
        ImGui::SetCursorScreenPos({cardPos.x, cardPos.y + cardH + 4.f});
        ImGui::Dummy({NOTIF_WIDTH, 0.f});
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(2);
}
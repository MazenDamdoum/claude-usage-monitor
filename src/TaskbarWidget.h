// The readout embedded in the Windows taskbar, the same way TrafficMonitor
// does it: a layered child window of Shell_TrayWnd placed just left of the
// notification area (and left of TrafficMonitor, if it is there).
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Settings.h"
#include "UsageView.h"

class TaskbarWidget {
public:
    static const UINT kMsgContextMenu = WM_APP + 20;  // posted to owner
    static const UINT kMsgDoubleClick = WM_APP + 21;  // posted to owner
    static const UINT kMsgColumnClick = WM_APP + 22;  // posted to owner; wParam = column index
    static const UINT kMsgBlossomClick = WM_APP + 23; // posted to owner: collapse to the blossom, or back
    static const UINT kMsgCheckClick = WM_APP + 24;   // posted to owner: check usage now

    void Init(HINSTANCE instance, HWND owner);
    // Which taskbar this readout lives on: 0 = the main one, 1, 2, ... = the other monitors' (left
    // to right). TaskbarCount: how many there are now.
    void SetTarget(int index) { target_ = index; }
    static int TaskbarCount();
    void Destroy();
    // Call about once a second: re-attaches if Explorer restarted, follows the
    // tray as it grows/shrinks, and repaints when anything visible changed.
    void Update(const std::vector<DisplayItem>& items, const Settings& settings);
    void SetTooltip(const std::wstring& text, bool enabled);
    // Multi-account view: a short tooltip per column and whether clicking it does something
    // (switch to that account). Empty = one tooltip for the whole readout.
    void SetColumns(const std::vector<std::wstring>& tips, const std::vector<bool>& clickable);
    // A green glow that fades in and out once over a column: the switch to it is confirmed.
    void Flash(int column, COLORREF color = RGB(52, 199, 89));  // red: "can't switch to it"
    // What Claude is writing: tokens per second, and the pile written in this stretch of work.
    struct TokenFlow {
        double rate = 0;
        double count = 0;
        bool working = false;
        long long pile = 0;  // changes when a new stretch begins
        bool count_up = false;  // show the pile ("1,474 tokens") instead of the speed ("117 tok/s")
        double left = -1;       // tokens left in the 5-hour window ("842.3k left"); -1 unknown
    };
    // Where it comes from. The widget redraws "N tok/s" (or the pile) and the Claude spinner after
    // it by itself: about 60 times a second while tokens flow, 10 when idle. The speed rolls up and
    // down smoothly; the pile, like Claude Code's counter, only climbs. The spinner turns faster the
    // more tokens flow, and stands still when nothing does.
    void SetTokenSource(std::function<TokenFlow()> source);
    // The usage checks with Anthropic (unix seconds): the last one and the next. Drawn at the far
    // right as a ring that empties toward the next check with the time left inside ("1:42"), then
    // turns ("now") until the check is done and the next one is set - as on the phone.
    struct CheckTimes { long long last = 0, next = 0; };
    void SetCheckSource(std::function<CheckTimes()> source);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool Attach();
    int ColumnAt(int x) const;
    void ShowTooltipText();
    int RightEdge(int dpi) const;
    int IconsRight(int dpi) const;  // where the taskbar's own buttons end
    void Animate();  // one frame of the token speed and its spinner

    HINSTANCE instance_ = nullptr;
    int target_ = 0;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND taskbar_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tooltip_text_;     // the whole-readout tooltip
    std::wstring shown_tip_;        // what the tooltip control currently shows
    std::vector<std::pair<int, int>> col_spans_;  // x range of each column
    std::vector<std::wstring> col_tips_;
    std::vector<bool> col_clickable_;
    int hover_col_ = -1;
    bool left_down_ = false;        // a left press began on the readout
    ULONGLONG left_down_at_ = 0;    // when (held half a second: the menu)
    bool down_was_right_ = false;   // the press already looked like a right-click (Ctrl, right flag)
    bool tooltip_enabled_ = true;
    bool tips_active_ = true;       // tips shown right now (off while folding or folded)

    HFONT font_ = nullptr;
    HFONT small_font_ = nullptr;    // the time inside the ring
    std::wstring font_key_;
    std::wstring render_key_;
    int flash_col_ = -1;
    COLORREF flash_color_ = RGB(52, 199, 89);
    ULONGLONG flash_start_ = 0;
    std::vector<Sprite> spin_sprites_;  // the spinner's frames, ink centred (Segoe UI Symbol)
    int spin_box_ = 0;              // their size
    int spin_w_ = 0;                // the widest frame's ink: the spinner's slot in the layout
    int digit_ink_ = 0;             // the middle of the digits' ink, from the top of their line
    int small_digit_ink_ = 0;       // the same in small_font_
    RECT blossom_hit_ = {};         // where a click on the blossom lands (empty when it isn't shown)
    // Collapsing to the blossom and back, animated: 0 = all shown, 1 = only the blossom.
    double fold_pos_ = 0, fold_from_ = 0, fold_target_ = -1;
    ULONGLONG fold_start_ = 0;
    bool folding_ = false;
    int letter_top_ = 0;            // where letters' ink starts below the top of their line
    std::function<TokenFlow()> token_source_;
    std::function<CheckTimes()> check_source_;
    RECT check_hit_ = {};           // the ring: a click checks now
    bool check_spinning_ = false;   // "now": the ring turns (its own timer)
    long long check_next_ = 0;      // the next check as last seen, and since when: the ring's full span
    long long check_since_ = 0;     // (the last check's own time can be from before a restart)
    double shown_rate_ = 0;         // eases toward the source: the spinner's pace
    double shown_count_ = 0;        // climbs toward the pile, never down
    long long shown_pile_ = -1;
    double spin_phase_ = 0;         // position in the spinner's frames
    UINT anim_ms_ = 0;              // the animation timer's interval, 0 = stopped
    ULONGLONG anim_last_ = 0;
    std::wstring token_text_ = L"0 tok/s";  // as drawn
    std::wstring token_speed_text_ = L"0 tok/s";  // the same without "· tok left"
    int fit_level_ = 0;             // how much the readout dropped to fit this taskbar (see Update)
    bool token_count_up_ = false;
    bool tokens_left_shown_ = false;  // "· 842.3k left" after the speed (its width is kept for it)
    int token_frame_ = -1;          // spinner frame as drawn; -1 = standing still
    double shown_tokens_w_ = 0;     // the tokens column's width as drawn: eases toward the text on screen (0 = not set yet)
    int tokens_w_goal_ = 0;         // what it is easing toward, from the last layout
    std::vector<DisplayItem> last_items_;  // to redraw the fade between the app's updates
    Settings last_settings_;
    bool have_last_ = false;
    RECT placed_ = {};
};

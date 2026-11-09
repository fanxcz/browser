#pragma once

// wvhost.h — WebView2 (Chromium) host window driven from the console UI.
// The console stays the command interface; this window renders real pages
// with full HTML/CSS/JS. All functions are thread-safe and queue their
// work until the window is ready.

#include <string>

namespace wv {

// Open (or reuse) the window and navigate it to url.
void Show(const std::wstring& url);

// Hide the window but keep it alive for the next Show().
void Hide();

// Window commands (queued until the controller is ready).
void Back();
void Forward();
void Reload();
void ZoomIn();
void ZoomOut();
void ZoomReset();
void External();   // hand the current URL to the system browser
void Find(const std::wstring& text);  // find-in-page highlight

// Destroy the window and stop the host thread. Call once at exit.
void Shutdown();

} // namespace wv

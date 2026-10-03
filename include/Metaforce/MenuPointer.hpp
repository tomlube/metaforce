#pragma once

namespace aurora::input {
struct InputEvent;
}

class CGuiFrame;
class CGuiTableGroup;
class CGuiWidget;
class CVector3f;

// Mouse and touch for the game's own menus (front end, pause screens, save and quit prompts).
// Clicks are hit-tested against the widgets of the menu tables as they were last drawn.
namespace metaforce::menu_pointer {

// Fed by the game input layer with mouse and touch events it sees while the cursor is free.
void HandleInputEvent(const aurora::input::InputEvent& event);
// Drops pending clicks and motion, e.g. when mouse look takes the cursor.
void Reset();
// Latches the events since the last call for this game frame. Called once per game frame when
// input is generated.
void BeginFrame();

// Records the camera a frame was just drawn with. Call right after the frame camera is set up.
void RecordFrameView(const CGuiFrame* frame, float alpha);
void ForgetFrame(const CGuiFrame* frame);

// The cursor moved this frame.
bool Moved();
// The left button (or a touch) went down this frame and nothing has taken the click yet.
bool LeftPressed();
// Takes the left click. Returns whether there was one.
bool ConsumeLeftPress();
// Takes the right click. Returns whether there was one.
bool ConsumeRightPress();
// Whole wheel steps this frame, positive down. Taking them leaves none for other tables.
int ConsumeScrollSteps();

// The selectable worker of the table under the cursor, or -1.
int HitTableWorker(CGuiTableGroup& table);
// Drags `owner` along the screen-space segment from `from` to `to` (world positions). A drag starts
// with a left press on `handle` or on the segment, and lasts while the button is held. Returns
// whether `owner` is being dragged, with the cursor's place on the segment in `t` (0 to 1).
bool DragAlong(const CGuiWidget& owner, const CGuiWidget& handle, const CVector3f& from,
               const CVector3f& to, float& t);

} // namespace metaforce::menu_pointer

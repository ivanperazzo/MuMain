#pragma once

// MainScene.h - Main game scene

#include "Core/Platform/WinCompat.h"

// Main scene lifecycle
void MoveMainScene();          // once per render frame: init, gating, UI, input
void MainSceneFixedUpdate();   // once per fixed 25 tps tick: world entity advance
bool RenderMainScene();

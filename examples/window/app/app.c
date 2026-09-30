#include "cels.h"
#include "composition/window.h"

/**
 * Declarative Application Root.
 * Attaches WindowComposition with WindowEval lifecycle evaluation.
 */
CEL_App(WindowApp, WindowComposition, WindowEval);


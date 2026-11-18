/**
 * The form that writes a run file.
 *
 * A terminal form and nothing else: no mouse, no colours beyond reverse video, no scrolling. What
 * it is for is the handful of decisions a bench run needs - which version, which set, which host -
 * and the guarantee that the file it leaves behind runs as it stands, in the same shape a person
 * would type by hand.
 */
#pragma once

#include "run_file.h"

namespace stand {

enum class FormAction
{
    Cancelled,
    Saved,
};

/**
 * @brief Open the form, save the run file on F2.
 *
 * @param spec Settings to start from, including the file to write.
 * @return whether the file was saved. On Saved, spec.file holds the path actually written.
 */
FormAction editRunFile(RunSpec& spec);

} // namespace stand

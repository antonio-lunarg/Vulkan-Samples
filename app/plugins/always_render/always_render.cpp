/* Copyright (c) 2024, LunarG, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "always_render.h"

#include "platform/parser.h"

namespace plugins
{
AlwaysRender::AlwaysRender() :
    AlwaysRenderTags("Always render",
                     "Render even when unfocused.",
                     {},
                     {&always_render_cmd})
{
}

bool AlwaysRender::is_active(const vkb::CommandParser &parser)
{
	return parser.contains(&always_render_cmd);
}

void AlwaysRender::init(const vkb::CommandParser &parser)
{
	platform->force_render(true);
}

}        // namespace plugins

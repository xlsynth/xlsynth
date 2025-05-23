// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Re-exports of sv_declarations with new names.

import sv_declarations_one;

pub type re_export_my_struct = sv_declarations_one::my_struct;
pub type re_export_my_enum = sv_declarations_one::my_enum;
pub type re_export_my_tuple = sv_declarations_one::my_tuple;

#[sv_type("rename_sv_struct")]
pub type rename_my_struct = sv_declarations_one::my_struct;
#[sv_type("rename_sv_enum")]
pub type rename_my_enum = sv_declarations_one::my_enum;
#[sv_type("rename_sv_tuple")]
pub type rename_my_tuple = sv_declarations_one::my_tuple;

#[sv_type("new_sv_tuple")]
pub type add_name_tuple = sv_declarations_one::non_sv_tuple;

// Copyright 2026 The XLS Authors
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
// BEGIN_CONFIG
// # proto-message: xls.fuzzer.CrasherConfigurationProto
// exception: "memory exhausted"
// issue: "https://github.com/google/xls/issues/4142"
// sample_options {
//   input_is_dslx: true
//   sample_type: SAMPLE_TYPE_FUNCTION
//   ir_converter_args: "--top=main"
//   ir_converter_args: "--lower_to_proc_scoped_channels=false"
//   convert_to_ir: true
//   optimize_ir: true
//   use_jit: true
//   codegen: true
//   codegen_args: "--nouse_system_verilog"
//   codegen_args: "--output_block_ir_path=sample.block.ir"
//   codegen_args: "--generator=pipeline"
//   codegen_args: "--pipeline_stages=2"
//   codegen_args: "--worst_case_throughput=1"
//   codegen_args: "--reset=rst"
//   codegen_args: "--reset_active_low=false"
//   codegen_args: "--reset_asynchronous=true"
//   codegen_args: "--reset_data_path=true"
//   simulate: true
//   simulator: "iverilog"
//   use_system_verilog: false
//   timeout_seconds: 1500
//   calls_per_sample: 128
//   proc_ticks: 0
//   known_failure {
//     tool: ".*codegen_main"
//     stderr_regex: ".*Impossible to schedule proc .* as specified; .*: cannot achieve the specified pipeline length.*"
//   }
//   known_failure {
//     tool: ".*codegen_main"
//     stderr_regex: ".*Impossible to schedule proc .* as specified; .*: cannot achieve full throughput.*"
//   }
//   with_valid_holdoff: false
//   codegen_ng: false
//   disable_unopt_interpreter: false
//   lower_to_proc_scoped_channels: false
// }
// inputs {
//   function_args {
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x31_c07d_f1c4_d6db_15a0_d27a_5338_cfe9_7220_9075_cac8_7e51_9c61_143f_1b86_881b_9eb6_6058_3fbe_1c50_6905_4e9f_9e96_0088_8783_13a7_69b3_850d_a9c8_dc87_1f00_1cf4_34d3_7504_94c2_b5d3_fd93_af4f_b6d9_3a31_f40d_54a2_4811_4648_49fb_5472_3dbc_2524_43ff_61f2_73ab; bits[2]:0x3"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x0; bits[2]:0x2"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x8000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x1_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x20_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x2_792b_eb6b_6f60_a332_8e87_09f5_8a5b_08d6_3594_296a_3a58_2c7d_2569_37bd_fff4_29e9_e487_b3bd_fc3c_19dc_4748_4848_704d_dc18_ea7a_2080_580a_51b7_f1b0_eb2f_c255_7fb4_84bf_afa0_d380_f1dd_3dc8_b087_3688_66b6_7c68_f045_fe48_4056_089c_b6f6_9f05_cda5_a41f_d157; bits[2]:0x1"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x0"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x0"
//     args: "bits[806]:0x0; bits[2]:0x3"
//     args: "bits[806]:0x3d_c242_3862_b00e_508a_5c80_18ee_a823_d7d7_617d_2024_0a02_862c_e615_10a0_b1ea_e0f7_7590_0ab7_b201_e993_f4f5_9e29_641b_5ae2_b6a0_d51c_215c_4956_870d_6da4_7f9d_b24d_7f04_6d1b_038f_b496_f115_c4ec_dd89_6e66_aeea_5909_322f_6684_be51_3072_f22e_b3e3_5373_636b; bits[2]:0x2"
//     args: "bits[806]:0x39_6b9d_06b2_4c2d_5220_4711_4665_f335_f3c5_4efe_e183_1748_7a81_3f04_875c_d1ce_3413_6eae_5cf5_2592_d037_8fdf_9dcd_4ae0_9335_1fd0_e50b_a11d_b7c0_1dbf_5f78_05fe_5115_553f_eccc_49f2_1ce6_eac0_2ca9_517a_5347_175a_99d0_1cb1_a5d8_f2c3_ac3e_b526_3711_4144_16e6; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x100_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x4_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x200_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x2"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x5_a4bd_cb8c_d91b_f45f_7d39_6764_17a6_e9a8_ffec_0d5a_c8e1_97ce_b934_be8d_faa3_0f0d_bd85_7bb7_8b6a_2a44_f16f_85f3_e3ab_cef9_fb3a_6cbe_a883_7af3_14e6_061a_bd57_0a56_93a4_fac6_9678_0376_2809_1c18_1664_386e_1ee1_1232_9b91_cdb9_c8cb_9e31_dde5_a6ac_673d_b0a6; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x400_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x3_5018_4c58_75a1_aea0_a2a2_26f9_fb47_f39c_6ce9_ca8e_b8d1_bb41_1279_9fe1_7305_375f_ae69_e306_5525_a5cf_0f61_a84d_db81_5dd3_06e6_e7c1_4e73_1e13_3fd0_cfff_3bb2_bbce_e98d_bae3_13c2_bce3_def7_e0f8_111e_2f37_d3c1_d4b6_82a2_4451_13e5_1f62_323e_90fd_3108_a0c6; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x3"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x12_b127_c95e_327c_6202_e5a6_71fd_1912_d098_51d7_0e18_4504_5d0f_e7de_3dd8_3251_e269_e522_36da_c426_1582_c8e0_a605_93b6_2522_8ac5_05e4_e46f_d89e_0632_30fd_dc56_4d62_9564_d2f7_dcef_6574_acb4_0609_21e3_c39a_b38f_1b75_8817_38c5_8e02_0c64_1b7f_3ea9_eaab_81c8; bits[2]:0x1"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x14_bc18_917a_d3aa_d6fc_dbf3_76d3_c9e6_5fa6_81f9_e24c_22c5_ff9b_9fa8_1104_60e7_c246_11de_0909_4a35_d5c2_21c0_b404_47d0_43ab_4dda_3e60_fdc0_2e5d_c1b6_0ecf_3473_daa6_e02d_4cda_9030_e6c8_0bac_f692_225c_e122_c001_d44f_30df_f723_a8c8_d510_1286_5cd3_ecb2_24e5; bits[2]:0x0"
//     args: "bits[806]:0x2_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x3"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x1000_0000; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x40_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x2000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x3"
//     args: "bits[806]:0x400_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x8_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x38_b21d_60e4_6f58_c1e3_e250_1fd0_e03d_a914_0848_1cde_eb4b_1f39_a45a_175d_895a_4373_0f80_ca11_7990_88ed_68a8_5879_5504_29da_b133_35de_e501_bb75_ab65_9165_06c7_a877_f2f8_c1da_3dee_4e54_d3ce_d9a9_4d77_cfcd_f4e9_c1c5_a887_4dbb_8fc6_233e_3634_493e_fb36_e61c; bits[2]:0x2"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x0; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x3"
//     args: "bits[806]:0x0; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x11_9df4_872f_d274_2594_6d96_0d09_d96f_e5ef_2c97_b195_841c_3dd0_b365_d99f_6874_4fd9_af70_6108_21f1_ef5f_2db0_9a49_a9d4_37d2_e17c_c6e3_aeee_b4a7_718e_faa9_caec_c891_2662_9c0d_8b8a_85f1_ceee_f5ab_59ad_cc92_3038_eb5b_5a98_6586_b780_1e2d_5802_e487_6442_dc6d; bits[2]:0x2"
//     args: "bits[806]:0xe_1bc8_f8a8_f42a_de87_6f81_cde9_5b18_b042_a75f_5efb_509c_f6f1_7773_8c57_6f7f_e745_bafd_a478_a350_e0b3_f335_0422_fb57_ad40_f03d_9a11_2806_7f1e_a7bc_12de_17ca_cbb0_33f2_91ed_47c0_04c8_62da_2bea_92d3_3bc6_12f4_8653_3c4e_caac_2668_69a8_ed6b_d263_70b6_79da; bits[2]:0x2"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x0"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x2_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x400; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x3"
//     args: "bits[806]:0x28_087a_19e5_ed50_fdad_71cc_2cd5_88e4_6ec9_3dad_9781_c12e_125a_5f3a_890d_1111_e08f_0e38_7f0d_d857_5873_6714_2e76_8e8c_0a4c_f084_b24b_93f5_1060_8bfc_ee1d_f523_a06c_677e_7686_fe2d_e6fd_3e14_da11_4e90_038f_d72b_a76e_8b4d_9ce0_f99e_72b8_10aa_a66e_67f5_e689; bits[2]:0x1"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x4_728b_b6e1_2248_08b3_20d5_aea0_d04b_63a0_f71a_9629_85b7_713b_6d9c_2748_a0b1_bbac_e146_d3ed_efd9_bf8d_7cd2_2c19_e037_46e5_7fa8_6a8b_65e2_0c76_02d9_e3d1_6981_a849_3b24_10de_c5a9_a50a_0c90_032b_402b_a216_86e5_e9c7_c1ea_e0b0_e72a_2c87_7045_4bee_ea08_2714; bits[2]:0x3"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x28_564a_6316_434a_c37d_3b05_07a7_9c08_dab3_a51e_d6a4_8dc8_b1c6_a418_270b_7a36_91c8_a7e4_c14d_4563_f135_3aa9_98a6_9857_9bcd_579b_8aea_0661_66be_cf42_d9ef_b459_3244_4d49_7179_d0e8_f4c5_ba79_ae9e_9af3_0e0d_3962_5aa9_f2d4_0c2c_f5e8_efc6_0969_f777_66e1_8299; bits[2]:0x1"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x2"
//     args: "bits[806]:0x0; bits[2]:0x2"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x2"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x40_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x10_0000_0000_0000_0000_0000; bits[2]:0x0"
//     args: "bits[806]:0x15_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555_5555; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x3"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x1"
//     args: "bits[806]:0x21_a2b2_451a_9eae_3bdc_a3a0_2279_f690_e2d3_8eb6_1dc3_cfe4_de12_d4c7_894b_e779_a9fb_fe97_74d6_4756_795c_7ed6_6164_8ea4_d7a2_06f2_2e1d_09c1_0fe7_184c_c2db_4e37_31e2_5884_c4dc_d1b8_addc_5678_7c32_082d_6c06_36ed_bdd5_1859_b466_375e_1a30_1e22_1a19_3ed8_a032; bits[2]:0x2"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x9_3437_8d4b_51da_55d7_293e_5fb0_5f63_293c_e7dd_a90a_8cda_ec0f_6472_4ee2_d813_0f8c_a586_c7a3_0f26_b78c_3bfd_bb80_8569_adcd_8414_252e_7bf0_c03d_0aff_38bf_47fe_3d67_a606_23db_1fab_7bc5_f428_0ca9_7ed1_8178_cdc3_d347_d08b_e540_8219_5626_b13b_10ee_1206_5e59; bits[2]:0x1"
//     args: "bits[806]:0x3f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x0"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x2"
//     args: "bits[806]:0x10_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x2"
//     args: "bits[806]:0x0; bits[2]:0x1"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x2"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x0"
//     args: "bits[806]:0x400_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x1f_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x0"
//     args: "bits[806]:0x2a_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa_aaaa; bits[2]:0x1"
//     args: "bits[806]:0x19_4081_9de8_3b2d_48ae_eec3_cf82_3072_c20d_8651_78e7_dabc_c649_7688_414f_7da5_b0f5_f81e_6981_06b2_2f11_3fbf_c8ef_957d_9273_6c14_c2fa_26ae_0ad4_4440_d850_9bdd_a483_c1ef_5687_507d_373b_e25e_65f7_99a0_7e62_78d2_9d01_c413_d636_bf69_d4e9_0563_83ba_8ae9_4a61; bits[2]:0x1"
//     args: "bits[806]:0x0; bits[2]:0x1"
//   }
// }
// 
// END_CONFIG
type x4 = xN[bool:0x0][8];
fn x21(x22: x4) -> (u2, s46, u2) {
    {
        let x23: u2 = x22[:-6];
        let x24: s46 = s46:0x1fff_ffff_ffff;
        (x23, x24, x23)
    }
}
fn main(x0: uN[806], x1: s2) -> (x4[1899], x4, uN[806]) {
    {
        let x2: uN[806] = !x0;
        let x3: uN[806] = x2 | x2;
        let x5: x4[1899] = ";CF!6EY:Kk7TM%JwYeY&,/X1sdi-v@L|>3%3ubp_di(g-(quscJq+)ksjyaZqH`Z1j> .w2g.LG8/(8<*p;OeU$Bw_g,A0wbed\'|`sWni!R*P[Or);G=b=/d0J5{:(A=-ih(UpYq<uY(bEkz@b)lr\"a17}\'_Uo\\\" Rd4{cD4l40<VgO\'(:2+wx[W.;{~J}T0N!s>\\R\'NYb.OwI\'[SoJ|*<;B\\DGd48q1O6AFz`3.q*J/[gVQXC@g]14qQzJ$4Dc~H%]<\\2&V<sp.\\z>+]=?Z[0&)DAGbE*w(Yi!E3iG\'[>!Qm,S>9:C%@ZQk:w.FGLRfCH#hk:vICMKLxG!h_!?)e#V[pm|XfH?0Jtg)aM qsl#gN~h1iEAL\"^T?(Y60nDL`S\'&0ts*R\'#YREuPj}k=\\8*~28qt|3%y1L*6jb}t,ZiSp4G.bk8}H;/*DpQDg[RPopqf*~7mI4:qftRWJmha&Wwl)D\\la9orf$avfu$-exV&?ZM7s^hNk6N4qbJ #M37{}j5o/8wgf0*A|<ZpRT*/]vjL3]^!m?u1YEv<&}iH}bT;G:lhORD%.4e4u/=]ErN]sF37lC2reajy)%;}]X4/{H[|+FD*xat!X9MF8N/& 7#Q8z9+Kf!fpG^g0=y595O>KOr\\<eI$K+EJhR3(bUhr<_MS>5_9l8WO_C]Q$$b2>q!7Rhy,hU@jRXSTM_j*<ftGSM3Oa!oNv@P} 2\"qK3s@~caBw1r=$0oX8^K=N~N(+i,{S#HAZ:$0<6alPC}S.gvA 1f3:8&NDCa2):x1kAc2TU_@VXRX[~=$F-+ SUMc_|6~bK(zT]E?(Qf YM=WwM!T6.Z@M6KWZ/msI M3#s>J}sS?R:{KXmv(3/Yd0(0Y4< eA-[Dv${WN!2sCLow-kXL<&/Ow%Q^l(JPWhz]^n|C4950}8KIq#1>26yVJLRD/C)@g^/,&4{{gs}WZ6GAgSKT#}f`~<.axXTpV7oRwM^$q&><A>\\=!WT1zh$`Vf@LO4nB,37Tba<I4=5THg@jo-=%<\'+$_ozyx.Bm0.3+$R0Mip7eM^34(a^1@qT$d=G+-/dciW]>7W@GlZw}Yf yhQi&O]?ygSwey`x:7dD}ADp`EkOD`ZdS>1|PQvYC:)y^UdJ?yU?\\u_\"n-/pgML~3 )f6B-Uas<FHUJn]2OJr$0QVdj$(RGM-\'bfR{4OEZ^R;Q-)Xgs&/C#]rPCEFb6N,T8yq\"F?J:3STSBa[9 \\\"}!q56*vA-Z69CL{?5b-Ggx*;ZEOAw@{~g.5jE33f^2Z@Y~<kXSp]lgX)&:$dJ0>VRb$5\"tK)g!qo_q<m|.b*|Qh2|@4-)YhRO7M%C(I[JnB1Gy\\ y0YoiS]#Q9E*A-bKBA(8-&EqZN:Ix`QT$/F8-i5Olb```r}:FwVAG{Pqq8M)mWWOj60%pKGOehh/^\\Aw813RmDM-eIXQXua..ejdNN[Nue]M^h,k2s$e2E o{e<YTqmWwu9&+u^vm3L4 tT<;?x_qK;iN$[$jIS87y~M|81zxozH`aZOFrxy/X~73ObvN\"FKg<ND-U@9#Y-6W|s^Kan#\"hjqe0IW):u%\'wb=#\\%dCKAGzUEW2foIVxn0:,n)(ts2iI$|nt4m|B/1AX97y&M=]<(8WVTsdN!Yyegw*W:d%-r~UTVQp7fIzl.f!fWIbmDt`e6KR\'&,nyf-%:EPeD)o_K#OW[8@HqgG%e.cN(ho~p<< Xe13,ew}P*}6Nl\'4,h`8!-k&l!\\)C:mtz\"7/q!O>;suh<C^~-)n}AsLi`GOH6$YL6ks1|8g$TyEcmRMc?|RWh9Y* L#oYX$MuqVZ& Sxe6&+!-9Iyy`$2WL!OYmu6p.SLS?~gm$9l(T(Zg|pnz0>zU4l\\_M9Ed<=woPP^W$gZ8k_|p=T&a3@3`nHn\';#fs";
        let x6: s2 = -x1;
        let x7: s2 = -x6;
        let x8: s2 = x6 + x7;
        let x10: uN[806] = {
            let x9: (uN[806], uN[806]) = umulp(x1 as uN[806], x2);
            x9.0 + x9.1
        };
        let x11: uN[806] = -x0;
        let x12: uN[806] = -x3;
        let x13: u24 = u24:0x55_5555;
        let x14: u21 = u21:0x0;
        let x15: uN[806] = clz(x2);
        let x16: x4 = x5[if x15 >= uN[806]:0x548 { uN[806]:0x548 } else { x15 }];
        let x17: x4[1899] = update(x5, if x13 >= u24:0x368 { u24:0x368 } else { x13 }, x16);
        let x18: x4 = !x16;
        let x19: x4 = x17[if x0 >= uN[806]:0x65d { uN[806]:0x65d } else { x0 }];
        let x20: u8 = x19[0:];
        let x25: uN[806] = x10 * x10;
        let x26: bool = x5 == x5;
        let x27: uN[806] = signex(x25, x15);
        (x17, x19, x25)
    }
}

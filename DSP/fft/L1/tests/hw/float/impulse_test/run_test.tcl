open_project prj_impulse_test_float
set_top fft_top
add_files src/data_path.h
add_files src/top_module.cpp -cflags "-I../../../../include/hw/xf_fft/float/"
add_files src/top_module.hpp
add_files -tb src/main.cpp -cflags "-I../../../../include/hw/xf_fft/float/"
open_solution "solution1-float"
set_part {xc7vx485tffg1157-1}
create_clock -period 3.3 -name default
csim_design -clean
csynth_design
cosim_design -trace_level port
#export_design -format ip_catalog
exit
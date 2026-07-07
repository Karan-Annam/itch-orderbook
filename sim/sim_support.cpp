// sim_support.cpp — legacy symbol some Verilated models still reference when
// built without SystemC. Defining it once here resolves the link for every
// custom test harness in this directory.
double sc_time_stamp() { return 0.0; }

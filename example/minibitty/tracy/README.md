

# Tracy Profiler

This subdirectory is a fork of the 
[Tracy Profiler](https://github.com/wolfpld/tracy).
As of 2025-02-20, [v0.11.1](https://github.com/wolfpld/tracy/tree/v0.11.1)
is the original source.  The minibitty example's adapter subcoommand
forwards the adapter trace information to Tracy when provided with 
the "--tracy" argument.  It functions as a Tracy client.

The [tracy.diff](tracy.diff) file contains the differences from the stock
Tracy release to this forked source.  This fork removes the actual profiling
code since it only needs to forward the already valid trace data.
[adapter_tracy.cpp](../adapter_tracy.cpp) 
reimplements the network communication to allow for correct data population. 


You can download the precompiled Windows binaries for 
[v0.11.1](https://github.com/wolfpld/tracy/releases/tag/v0.11.1).

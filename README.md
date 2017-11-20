# Graph-viz for Score-P

This plugin writes graphviz files for each location that can be analyzed later.

This work has been presented in [DOI doi.org/10.1007/978-3-319-56702-0_4, Extending the Functionality of Score-P Through Plugins: Interfaces and Use Cases](http://doi.org/10.1007/978-3-319-56702-0_4).

The resulting graphviz files will be exported to the experiment directory.

[![build status](https://api.travis-ci.org/rschoene/scorep_substrate_graphviz.svg)](https://travis-ci.org/rschoene/scorep_substrate_graphviz)

## Compilation and Installation

### Prerequisites

To compile this plugin, you need:

* C compiler (with `--std=c11` support)

* CMake

* Score-P installation

### Building

1. Create a build directory

        mkdir build
        cd build

2. Invoke CMake

        cmake ..

    If your Score-P installation is in a non-standard path, you have to manually pass that path to
    CMake:

        cmake .. -DSCOREP_CONFIG=<PATH_TO_YOUR_SCOREP_ROOT_DIR>/bin/scorep-config

    If you only have exclusive functions, e.g. OpenMP and MPI, you can get rid of the enter/exit depiction,
    and have one graphviz-node per function. To tell cmake, just use
    
        cmake .. -DOPENMP_MPI=ON

    This plugin can distinguish calls to functions based on their stack.
    This option should not be disabled when the only enter option is used.
        cmake .. -DDYNAMIC_ID=OFF

3. Invoke make

        make

4. Copy the resulting library to a directory listed in `LD_LIBRARY_PATH` or add the current path to
    `LD_LIBRARY_PATH` with

        export LD_LIBRARY_PATH=`pwd`:$LD_LIBRARY_PATH

## Usage

In order to use this plugin, you have to add it to the `SCOREP_SUBSTRATES_PLUGINS` environment
variable.

    export SCOREP_SUBSTRATE_PLUGINS="graphviz"

The configuration of the plugin is done via environment variables.

### Environment variables
    
    
### Known issues
The gcc backtrace function comes with some overhead. If you want to avoid it, you can pass the cmake option -DDYNAMIC_ID=OFF.

### If anything fails

1. Check whether the plugin library can be loaded from the `LD_LIBRARY_PATH`

2. Check whether you provide sane values for the environment variables.

3. Open an [issue on Github](https://github.com/Ferruck/scorep_substrates_dynamic_filtering/issues).

## License

For information regarding the license of this plugin, see
[LICENSE](https://github.com/rschoene/scorep_substrate_graphviz/blob/master/LICENSE)

## Author

* Robert Schoene (robert dot schoene at tu-dresden dot de)

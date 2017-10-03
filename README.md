NOTE: please fill in the first section with information about your game.

# *Pool Dozer*

*Pool Dozer* is *Breeanna Ebert*'s implementation of [*Design Document*](http://graphics.cs.cmu.edu/courses/15-466-f17/game3-designs/jmccann) for game3 in 15-466-f17.

![screenshot](https://github.com/turbotortoise/15-466-f17-base3/blob/master/screenshots/Game3Screenshot.png)

## Asset Pipeline

*Meshes are exported through the export-pool-meshes.py script through blender. All meshes in which the user doesn't interact with are added to the scene. Interactable objects are added with references.*

## Architecture

*I created vectors of cylinders, pool balls, and dozers. This way, the items are accessible as they are loaded into the scene. I case on the button inputs in order to alter each dozer's rotation and speed. Each dozer can collide with each pool ball, the opposing dozer, and the cylinders. The balls can collide with each other and the dozers.*

## Reflection

*Collision detection between the dozers and balls are accurate. However, the friction used on the balls makes the balls appear to move in a rigid manner. I attempted to add some 3D physics such as gravity, and it would have been nice to add bounce back from collision impact.*


# About Base2

This game is based on Base2, starter code for game2 in the 15-466-f17 course. It was developed by Jim McCann, and is released into the public domain.

## Requirements

 - modern C++ compiler
 - glm
 - libSDL2
 - libpng
 - blender (for mesh export script)

On Linux or OSX these requirements should be available from your package manager without too much hassle.

## Building

This code has been set up to be built with [FT jam](https://www.freetype.org/jam/).

### Getting Jam

For more information on Jam, see the [Jam Documentation](https://www.perforce.com/documentation/jam-documentation) page at Perforce, which includes both reference documentation and a getting started guide.

On unixish OSs, Jam is available from your package manager:
```
	brew install ftjam #on OSX
	apt get ftjam #on Debian-ish Linux
```

On Windows, you can get a binary [from sourceforge](https://sourceforge.net/projects/freetype/files/ftjam/2.5.2/ftjam-2.5.2-win32.zip/download),
and put it somewhere in your `%PATH%`.
(Possibly: also set the `JAM_TOOLSET` variable to `VISUALC`.)

### Bulding
Open a terminal (on windows, a Visual Studio Command Prompt), change to this directory, and type:
```
	jam
```

### Building (local libs)

Depending on your OSX, clone 
[kit-libs-linux](https://github.com/ixchow/kit-libs-linux),
[kit-libs-osx](https://github.com/ixchow/kit-libs-osx),
or [kit-libs-win](https://github.com/ixchow/kit-libs-win)
as a subdirectory of the current directory.

The Jamfile sets up library and header search paths such that local libraries will be preferred over system libraries.

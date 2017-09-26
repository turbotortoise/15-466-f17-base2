#include "load_save_png.hpp"
#include "GL.hpp"
#include "Meshes.hpp"
#include "Scene.hpp"
#include "read_chunk.hpp"

#include <SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <fstream>

static GLuint compile_shader(GLenum type, std::string const &source);
static GLuint link_program(GLuint vertex_shader, GLuint fragment_shader);

int main(int argc, char **argv) {
	//Configuration:
	struct {
		std::string title = "Game2: Scene";
		glm::uvec2 size = glm::uvec2(640, 480);
	} config;

	//------------  initialization ------------

	//Initialize SDL library:
	SDL_Init(SDL_INIT_VIDEO);

	//Ask for an OpenGL context version 3.3, core profile, enable debug:
	SDL_GL_ResetAttributes();
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

	//create window:
	SDL_Window *window = SDL_CreateWindow(
		config.title.c_str(),
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		config.size.x, config.size.y,
		SDL_WINDOW_OPENGL /*| SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI*/
	);

	if (!window) {
		std::cerr << "Error creating SDL window: " << SDL_GetError() << std::endl;
		return 1;
	}

	//Create OpenGL context:
	SDL_GLContext context = SDL_GL_CreateContext(window);

	if (!context) {
		SDL_DestroyWindow(window);
		std::cerr << "Error creating OpenGL context: " << SDL_GetError() << std::endl;
		return 1;
	}

	#ifdef _WIN32
	//On windows, load OpenGL extensions:
	if (!init_gl_shims()) {
		std::cerr << "ERROR: failed to initialize shims." << std::endl;
		return 1;
	}
	#endif

	//Set VSYNC + Late Swap (prevents crazy FPS):
	if (SDL_GL_SetSwapInterval(-1) != 0) {
		std::cerr << "NOTE: couldn't set vsync + late swap tearing (" << SDL_GetError() << ")." << std::endl;
		if (SDL_GL_SetSwapInterval(1) != 0) {
			std::cerr << "NOTE: couldn't set vsync (" << SDL_GetError() << ")." << std::endl;
		}
	}

	//Hide mouse cursor (note: showing can be useful for debugging):
	//SDL_ShowCursor(SDL_DISABLE);

	//------------ opengl objects / game assets ------------

	//shader program:
	GLuint program = 0;
	GLuint program_Position = 0;
	GLuint program_Normal = 0;
	GLuint program_mvp = 0;
	GLuint program_itmv = 0;
	GLuint program_to_light = 0;
	{ //compile shader program:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 mvp;\n"
			"uniform mat3 itmv;\n"
			"in vec4 Position;\n"
			"in vec3 Normal;\n"
			"out vec3 normal;\n"
			"void main() {\n"
			"	gl_Position = mvp * Position;\n"
			"	normal = itmv * Normal;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 to_light;\n"
			"in vec3 normal;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	float light = max(0.0, dot(normalize(normal), to_light));\n"
			"	fragColor = vec4(light * vec3(1.0, 1.0, 1.0), 1.0);\n"
			"}\n"
		);

		program = link_program(fragment_shader, vertex_shader);

		//look up attribute locations:
		program_Position = glGetAttribLocation(program, "Position");
		if (program_Position == -1U) throw std::runtime_error("no attribute named Position");
		program_Normal = glGetAttribLocation(program, "Normal");
		if (program_Normal == -1U) throw std::runtime_error("no attribute named Normal");

		//look up uniform locations:
		program_mvp = glGetUniformLocation(program, "mvp");
		if (program_mvp == -1U) throw std::runtime_error("no uniform named mvp");
		program_itmv = glGetUniformLocation(program, "itmv");
		if (program_itmv == -1U) throw std::runtime_error("no uniform named itmv");

		program_to_light = glGetUniformLocation(program, "to_light");
		if (program_to_light == -1U) throw std::runtime_error("no uniform named to_light");
	}

	//------------ meshes ------------

	Meshes meshes;

	{ //add meshes to database:
		Meshes::Attributes attributes;
		attributes.Position = program_Position;
		attributes.Normal = program_Normal;

		meshes.load("meshes.blob", attributes);
	}
	
	//------------ scene ------------

	Scene scene;
	//set up camera parameters based on window:
	scene.camera.fovy = glm::radians(60.0f);
	scene.camera.aspect = float(config.size.x) / float(config.size.y);
	scene.camera.near = 0.01f;
	//(transform will be handled in the update function below)

	//add some objects from the mesh library:
	auto add_object = [&](std::string const &name, glm::vec3 const &position, glm::quat const &rotation, glm::vec3 const &scale) -> Scene::Object & {
		Mesh const &mesh = meshes.get(name);
		scene.objects.emplace_back();
		Scene::Object &object = scene.objects.back();
		object.transform.position = position;
		object.transform.rotation = rotation;
		object.transform.scale = scale;
		object.vao = mesh.vao;
		object.start = mesh.start;
		object.count = mesh.count;
		object.program = program;
		object.program_mvp = program_mvp;
		object.program_itmv = program_itmv;
		return object;
	};


	{ //read objects to add from "scene.blob":
		std::ifstream file("scene.blob", std::ios::binary);

		std::vector< char > strings;
		//read strings chunk:
		read_chunk(file, "str0", &strings);

		{ //read scene chunk, add meshes to scene:
			struct SceneEntry {
				uint32_t name_begin, name_end;
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
			};
			static_assert(sizeof(SceneEntry) == 48, "Scene entry should be packed");

			std::vector< SceneEntry > data;
			read_chunk(file, "scn0", &data);

			for (auto const &entry : data) {
				if (!(entry.name_begin <= entry.name_end && entry.name_end <= strings.size())) {
					throw std::runtime_error("index entry has out-of-range name begin/end");
				}
				std::string name(&strings[0] + entry.name_begin, &strings[0] + entry.name_end);
				if ((name != "Balloon1") && (name != "Balloon2") && (name != "Balloon3") &&
					(name != "Base") && (name != "Link1") && (name != "Link2") && (name != "Link3"))
				add_object(name, entry.position, entry.rotation, entry.scale);
			}
		}
	}

	//robot arm stack:
	std::vector< Scene::Object * > tree_stack;

	tree_stack.emplace_back( &add_object("Base", glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );
	tree_stack.emplace_back( &add_object("Link1", glm::vec3(0.0f, 0.0f, 0.2f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );
	tree_stack.emplace_back( &add_object("Link2", glm::vec3(0.0f, 0.0f, 1.3f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );
	tree_stack.emplace_back( &add_object("Link3", glm::vec3(0.0f, 0.0f, 1.3f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );

	for (uint32_t i = 1; i < tree_stack.size(); ++i) {
		tree_stack[i]->transform.set_parent(&tree_stack[i-1]->transform);
	}

	std::vector< float > wave_rotation(tree_stack.size(), 0.0f);
	std::vector< float > bottom_wave_acc(tree_stack.size(), 0.0f);
	std::vector< float > mid_wave_acc(tree_stack.size(), 0.0f);
	std::vector< float > top_wave_acc(tree_stack.size(), 0.0f);

	//balloon variables
	bool b1_is_popped = false;
	float b1_pop_time = -1.0f;

	bool b2_is_popped = false;
	float b2_pop_time = -1.0f;

	bool b3_is_popped = false;
	float b3_pop_time = -1.0f;

	float balloon_radius = 2.0f;

	glm::vec3 balloon_1_origpos = glm::vec3(2.0f, 1.0f, 3.0f);
	glm::vec3 balloon_2_origpos = glm::vec3(-2.0f, 1.0f, 2.0f);
	glm::vec3 balloon_3_origpos = glm::vec3(-2.0f, -1.0f, 2.0f);

	float balloon_Speed = 1.0f;
	float balloon_height_center = 2.5f; //center height for all balloons

	//ballon stack
	std::vector< Scene::Object * > balloon_stack;

	balloon_stack.emplace_back( &add_object("Balloon1", balloon_1_origpos, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );
	balloon_stack.emplace_back( &add_object("Balloon2", balloon_2_origpos, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );
	balloon_stack.emplace_back( &add_object("Balloon3", balloon_3_origpos, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)) );

	std::vector< float > balloon_movement(balloon_stack.size(), 0.0f);
	glm::vec2 mouse = glm::vec2(0.0f, 0.0f); //mouse position in [-1,1]x[-1,1] coordinates

	struct {
		float radius = 10.0f;
		float elevation = 0.0f;
		float azimuth = 0.0f;
		glm::vec3 target = glm::vec3(0.0f, 0.0f, 0.0f);
	} camera;

	//Variables for keeping track of arm movement
	//z or x
	float rotateVelocity = 0.0f;
	float rotateSpeed = 0.2f;
	//a or s
	float bottom_arm_rotateSpeed = 0.3f;
	float bottom_arm_velocity = 0.0f;
	//; or '
	float mid_arm_rotateSpeed = 0.4f;
	float mid_arm_velocity = 0.0f;
	//. or /
	float top_arm_rotateSpeed = 0.5f;
	float top_arm_velocity = 0.0f;


	//------------ game loop ------------

	bool should_quit = false;
	while (true) {
		static SDL_Event evt;
		while (SDL_PollEvent(&evt) == 1) {
			//handle input:
			if (evt.type == SDL_MOUSEMOTION) {
				glm::vec2 old_mouse = mouse;
				mouse.x = (evt.motion.x + 0.5f) / float(config.size.x) * 2.0f - 1.0f;
				mouse.y = (evt.motion.y + 0.5f) / float(config.size.y) *-2.0f + 1.0f;
				if (evt.motion.state & SDL_BUTTON(SDL_BUTTON_LEFT)) {
					camera.elevation += -2.0f * (mouse.y - old_mouse.y);
					camera.azimuth += -2.0f * (mouse.x - old_mouse.x);
				}
			} else if (evt.type == SDL_MOUSEBUTTONDOWN) {
			} else if (evt.type == SDL_KEYDOWN) {
				if (evt.key.keysym.sym == SDLK_ESCAPE)
					should_quit = true;

				//Button Inputs

				//rotate clockwise
				else if (evt.key.keysym.sym == SDLK_z) {
					rotateVelocity = 1.0f;
				}
				//rotate counterclockwise
				else if (evt.key.keysym.sym == SDLK_x) {
					rotateVelocity = -1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_a) {
					bottom_arm_velocity = 1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_s) {
					bottom_arm_velocity = -1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_SEMICOLON) {
					mid_arm_velocity = 1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_QUOTE) {
					mid_arm_velocity = -1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_PERIOD) {
					top_arm_velocity = 1.0f;
				}
				else if (evt.key.keysym.sym == SDLK_SLASH) {
					top_arm_velocity = -1.0f;
				}
			} else if (evt.type == SDL_KEYUP) {
				if ((evt.key.keysym.sym == SDLK_z) || (evt.key.keysym.sym == SDLK_x)) {
					rotateVelocity = 0.0f;
				}
				else if ((evt.key.keysym.sym == SDLK_a) || (evt.key.keysym.sym == SDLK_s)) {
					bottom_arm_velocity = 0.0f;
				}
				else if ((evt.key.keysym.sym == SDLK_SEMICOLON) || (evt.key.keysym.sym == SDLK_QUOTE)) {
					mid_arm_velocity = 0.0f;
				}
				else if ((evt.key.keysym.sym == SDLK_PERIOD) || (evt.key.keysym.sym == SDLK_SLASH)) {
					top_arm_velocity = 0.0f;
				}
			} else if (evt.type == SDL_QUIT) {
				should_quit = true;
				break;
			}
		}
		if (should_quit) break;

		auto current_time = std::chrono::high_resolution_clock::now();
		static auto previous_time = current_time;
		float elapsed = std::chrono::duration< float >(current_time - previous_time).count();
		previous_time = current_time;

		{ //update game state:

			//check balloon states
			if (!b1_is_popped) {
				glm::vec3 balloon_pos = balloon_stack[0]->transform.position;
				glm::vec3 needle_pos = tree_stack[tree_stack.size() - 1]->transform.position;
				float dist = std::sqrt(std::pow((balloon_pos.x - needle_pos.x), 2) + 
					                   std::pow((balloon_pos.y - needle_pos.y), 2) + 
					                   std::pow((balloon_pos.z - needle_pos.z), 2));
				printf("needlepos: (%f, %f, %f)\n", needle_pos.x, needle_pos.y, needle_pos.z);
				printf("dist: %f\n", dist);
				if (dist <= balloon_radius) {
					printf("b1 popped\n");
					b1_is_popped = true;
				}
			}

			if (!b2_is_popped) {
				glm::vec3 balloon_pos = balloon_stack[1]->transform.position;
				glm::vec3 needle_pos = tree_stack[tree_stack.size() - 1]->transform.position;
				float dist = std::sqrt(std::pow((balloon_pos.x - needle_pos.x), 2) + 
					                   std::pow((balloon_pos.y - needle_pos.y), 2) + 
					                   std::pow((balloon_pos.z - needle_pos.z), 2));
				if (dist <= balloon_radius) {
					printf("b2 popped\n");
					b2_is_popped = true;
				}
			}

			if (!b3_is_popped) {
				glm::vec3 balloon_pos = balloon_stack[2]->transform.position;
				glm::vec3 needle_pos = tree_stack[tree_stack.size() - 1]->transform.position;
				float dist = std::sqrt(std::pow((balloon_pos.x - needle_pos.x), 2) + 
					                   std::pow((balloon_pos.y - needle_pos.y), 2) + 
					                   std::pow((balloon_pos.z - needle_pos.z), 2));
				if (dist <= balloon_radius) {
					printf("b3 popped\n");
					b3_is_popped = true;
				}
			}

			//update balloon positions
			balloon_stack[0]->transform.position.z = balloon_height_center + ((std::sin(0.01f * elapsed)));
			//printf("stack0z: %f\n", balloon_stack[0]->transform.position.z);
			//upate arm positions
			for (uint32_t i = 0; i < tree_stack.size(); i++) {
				wave_rotation[i] += elapsed * (rotateVelocity * rotateSpeed);
				//wave_rotation[i] -= std::floor(wave_rotation[i]);
				float ang = (0.5f * float(M_PI)) * i;
				tree_stack[i]->transform.rotation = glm::angleAxis(
					std::sin(wave_rotation[i] * float(M_PI)),
					glm::vec3(2.0f*std::sin(ang), 0.0f, std::cos(ang))
				);
			}
			for (uint32_t i = 1; i < tree_stack.size(); i++) {
				float speed = bottom_arm_velocity * bottom_arm_rotateSpeed;
				bottom_wave_acc[i] += elapsed * speed;
				bottom_wave_acc[i] -= std::floor(bottom_wave_acc[i]);
				//printf("bottomwave: %f\n", (bottom_wave_acc[i] * float(M_PI)));
				float ang = (0.5f * float(M_PI)) * i;
				tree_stack[i]->transform.rotation = glm::angleAxis(
					std::sin(bottom_wave_acc[i] * float(M_PI)),
					glm::vec3(std::sin(ang), std::cos(ang), 0.0f)
					);
			}
			for (uint32_t i = 2; i < tree_stack.size(); i++) {				
				float speed = mid_arm_velocity * mid_arm_rotateSpeed;
				mid_wave_acc[i] += elapsed * speed;
				mid_wave_acc[i] -= std::floor(mid_wave_acc[i]);
				mid_wave_acc[i] += elapsed * (mid_arm_velocity * mid_arm_rotateSpeed);
				mid_wave_acc[i] -= std::floor(mid_wave_acc[i]);
				float ang = (0.5f * float(M_PI)) * i;
				tree_stack[i]->transform.rotation = glm::angleAxis(
					std::sin(mid_wave_acc[i] * float(M_PI)),
					glm::vec3(std::cos(ang), 0.0f, std::sin(ang))
					);
			}			
			for (uint32_t i = 3; i < tree_stack.size(); i++) {
				float speed = top_arm_velocity * top_arm_rotateSpeed;
				top_wave_acc[i] += elapsed * speed;
				top_wave_acc[i] -= std::floor(top_wave_acc[i]);
				top_wave_acc[i] += elapsed * (top_arm_velocity * top_arm_rotateSpeed);
				top_wave_acc[i] -= std::floor(top_wave_acc[i]);
				float ang = (0.5f * float(M_PI)) * i;
				tree_stack[i]->transform.rotation = glm::angleAxis(
					std::sin(top_wave_acc[i] * float(M_PI)),
					glm::vec3(std::sin(ang), std::cos(ang), 0.0f)
					);
			}
			

			//camera:
			scene.camera.transform.position = camera.radius * glm::vec3(
				std::cos(camera.elevation) * std::cos(camera.azimuth),
				std::cos(camera.elevation) * std::sin(camera.azimuth),
				std::sin(camera.elevation)) + camera.target;

			glm::vec3 out = -glm::normalize(camera.target - scene.camera.transform.position);
			glm::vec3 up = glm::vec3(0.0f, 0.0f, 1.0f);
			up = glm::normalize(up - glm::dot(up, out) * out);
			glm::vec3 right = glm::cross(up, out);
			
			scene.camera.transform.rotation = glm::quat_cast(
				glm::mat3(right, up, out)
			);
			scene.camera.transform.scale = glm::vec3(1.0f, 1.0f, 1.0f);
		}

		//draw output:
		glClearColor(0.5, 0.5, 0.5, 0.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);


		{ //draw game state:
			glUseProgram(program);
			glUniform3fv(program_to_light, 1, glm::value_ptr(glm::normalize(glm::vec3(0.0f, 1.0f, 10.0f))));
			scene.render();
		}


		SDL_GL_SwapWindow(window);
	}


	//------------  teardown ------------

	SDL_GL_DeleteContext(context);
	context = 0;

	SDL_DestroyWindow(window);
	window = NULL;

	return 0;
}



static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = source.size();
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, info_log.size(), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}

static GLuint link_program(GLuint fragment_shader, GLuint vertex_shader) {
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	GLint link_status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &link_status);
	if (link_status != GL_TRUE) {
		std::cerr << "Failed to link shader program." << std::endl;
		GLint info_log_length = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetProgramInfoLog(program, info_log.size(), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		throw std::runtime_error("Failed to link program");
	}
	return program;
}

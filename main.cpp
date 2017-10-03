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
		glm::uvec2 size = glm::uvec2(1000, 700);
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

	auto object_is_cylinder = [&](std::string const &name) {
		//printf("name: %s\n", name);
		if (name.find("Cylinder") != std::string::npos)
			return true;
		return false;
	};

	auto object_is_ball = [&](std::string const &name) {
		if (name.find("Ball") != std::string::npos)
			return true;
		return false;
	};

	auto object_is_dozer = [&](std::string const &name) {
		if (name.find("Circle") != std::string::npos)
			return true;
		return false;
	};

	std::vector< Scene::Object * > ball_object_list;
	std::vector< Scene::Object * > dozer_object_list;
	std::vector< Scene::Object * > cylinder_object_list;
	std::vector< int > dozer1_wheel_dir(4, 0);
	std::vector< int > dozer2_wheel_dir(4, 0);
	std::vector< float > dozer_rotation(2, 0.0f);
	std::vector< float > ball_rotation(ball_object_list.size(), 0.0f);
	float collision_radius = 0.15f; //collision radius of balls and dozers
	float score_collision_radius = 0.4f; //collision radius of cylinders


	//The only things constant in this world
	float gravity = 0.0098f;
	float air_damping = -1.0f;
	float friction = 0.9f;


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
				//place objects in the background
				if (object_is_cylinder(name))
					cylinder_object_list.emplace_back( &add_object(name, entry.position, entry.rotation, entry.scale));
				else if (object_is_ball(name))
					ball_object_list.emplace_back( &add_object(name, entry.position, entry.rotation, entry.scale));
				else if (object_is_dozer(name))
					dozer_object_list.emplace_back( &add_object(name, entry.position, entry.rotation, entry.scale));
				else
					add_object(name, entry.position, entry.rotation, entry.scale);
			}
		}
	}

	glm::vec2 mouse = glm::vec2(0.0f, 0.0f); //mouse position in [-1,1]x[-1,1] coordinates

	struct {
		float radius = 5.0f;
		float elevation = 1.57f;
		float azimuth = 1.57f;
		glm::vec3 target = glm::vec3(0.0f, 0.0f, 0.0f);
	} camera;

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
				//Uint8 *keystate = SDL_GetKeyState(NULL);
				if (evt.key.keysym.sym == SDLK_ESCAPE)
					should_quit = true;

				//Button Inputs

				//Controls for first dozer
				if (evt.key.keysym.sym == SDLK_a) {
					dozer1_wheel_dir[0] = 1;
				}
				if (evt.key.keysym.sym == SDLK_z) {
					dozer1_wheel_dir[1] = 1;
				}
				if (evt.key.keysym.sym == SDLK_s) {
					dozer1_wheel_dir[2] = 1;
				}
				if (evt.key.keysym.sym == SDLK_x) {
					dozer1_wheel_dir[3] = 1;
				}

				//Secondary dozer inputs
				if (evt.key.keysym.sym == SDLK_SEMICOLON) {
					dozer2_wheel_dir[0] = 1;
				}
				if (evt.key.keysym.sym == SDLK_PERIOD) {
					dozer2_wheel_dir[1] = 1;
				}
				if (evt.key.keysym.sym == SDLK_QUOTE) {
					dozer2_wheel_dir[2] = 1;
				}
				if (evt.key.keysym.sym == SDLK_SLASH) {
					dozer2_wheel_dir[3] = 1;
				}

			} else if (evt.type == SDL_KEYUP) {

				if (evt.key.keysym.sym == SDLK_a) {
					dozer1_wheel_dir[0] = 0;
				}
				if (evt.key.keysym.sym == SDLK_z) {
					dozer1_wheel_dir[1] = 0;
				}
				if (evt.key.keysym.sym == SDLK_s) {
					dozer1_wheel_dir[2] = 0;
				}
				if (evt.key.keysym.sym == SDLK_x) {
					dozer1_wheel_dir[3] = 0;
				}
				if (evt.key.keysym.sym == SDLK_SEMICOLON)
					dozer2_wheel_dir[0] = 0;
					
				if(evt.key.keysym.sym == SDLK_PERIOD) {
					dozer2_wheel_dir[1] = 0;
				}
				if (evt.key.keysym.sym == SDLK_QUOTE) {
					dozer2_wheel_dir[2] = 0;
				}
				if (evt.key.keysym.sym == SDLK_SLASH) {
					dozer2_wheel_dir[3] = 0;
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

			//Update Dozer positions
			for (uint32_t i = 0; i < 2; i++) {


				//Update dozer 1
				if (dozer1_wheel_dir[0] == 1) {
					//A button
					dozer_object_list[0]->transform.speed = 0.01f;
					dozer_rotation[0] += 0.01f;
					//dozer_object_list[0]->transform.rotation.z += std::sin(ang);
				}
				if (dozer1_wheel_dir[1] == 1) {
					//Z button
					dozer_object_list[0]->transform.speed = -0.01f;
					dozer_rotation[0] -= 0.01f;
				}
				if (dozer1_wheel_dir[2] == 1) {
					//S button
					dozer_object_list[0]->transform.speed = 0.01f;
					dozer_rotation[0] -= 0.01f;
				}
				if (dozer1_wheel_dir[3] == 1) {
					//X button
					dozer_object_list[0]->transform.speed = -0.01f;
					dozer_rotation[0] += 0.01f;
				}
				if ((dozer1_wheel_dir[0] == 0) && (dozer1_wheel_dir[1] == 0) && (dozer1_wheel_dir[2] == 0) && (dozer1_wheel_dir[3] == 0)) {
					//no buttons
					dozer_object_list[0]->transform.speed = 0.0f;
				}

				//Update Dozer 2
				if (dozer2_wheel_dir[0] == 1) {
					//semi-colon button
					dozer_object_list[1]->transform.speed = 0.01f;
					dozer_rotation[1] += 0.01f;
					//dozer_object_list[1]->transform.rotation.z += std::sin(ang);
				}
				if (dozer2_wheel_dir[1] == 1) {
					//period button
					dozer_object_list[1]->transform.speed = -0.01f;
					dozer_rotation[1] -= 0.01f;
				}
				if (dozer2_wheel_dir[2] == 1) {
					//quote button
					dozer_object_list[1]->transform.speed = 0.01f;
					dozer_rotation[1] -= 0.01f;
				}
				if (dozer2_wheel_dir[3] == 1) {
					//slash button
					dozer_object_list[1]->transform.speed = -0.01f;
					dozer_rotation[1] += 0.01f;
				}
				if ((dozer2_wheel_dir[0] == 0) && (dozer2_wheel_dir[1] == 0) && (dozer2_wheel_dir[2] == 0) && (dozer2_wheel_dir[3] == 0)) {
					//no buttons
					dozer_object_list[1]->transform.speed = 0.0f;
				}

				float ang = dozer_rotation[i] * float(M_PI);

				//Update Dozer positions
				dozer_object_list[i]->transform.velocity = glm::vec3(
					std::cos(ang), std::sin(ang), 0.0f);

				dozer_object_list[i]->transform.rotation = glm::angleAxis(
					std::sin(ang * dozer_object_list[i]->transform.speed),
					glm::vec3(0.0f, 0.0f, std::sin(ang) + std::cos(ang))
					);

				dozer_object_list[i]->transform.position += (
					dozer_object_list[i]->transform.speed * 
					glm::vec3(std::cos(ang), std::sin(ang), 0.0f)
					);

			}

			//Update Ball positions

			//Collision between dozer and ball
			auto dozer_collision = [&](Scene::Object *dozer, Scene::Object *ball) {
				//find distance
				float distance = std::sqrt(std::pow(ball->transform.position.x - dozer->transform.position.x, 2)
								 + std::pow(ball->transform.position.y - dozer->transform.position.y, 2));
				if (distance <= (2.0f * collision_radius)) {
					glm::vec3 a_to_b = ball->transform.position - dozer->transform.position;
					glm::vec3 norm_ab = std::sqrt(std::pow(a_to_b.x, 2) + 
												  std::pow(a_to_b.y, 2) + 
												  std::pow(a_to_b.z, 2)) * a_to_b;
					ball->transform.speed = dozer->transform.speed;
					ball->transform.velocity += 100.0f * dozer->transform.speed * norm_ab;
				}
			};

			//Elastic sphere collision
			auto sphere_collision = [&](Scene::Object *ball_1, Scene::Object *ball_2) {
				//find distance
				float distance = std::sqrt(std::pow(ball_2->transform.position.x - ball_1->transform.position.x, 2)
								 + std::pow(ball_2->transform.position.y - ball_1->transform.position.y, 2));
				if (distance <= (2.0f * collision_radius)) {
					glm::vec3 a_to_b = ball_2->transform.position - ball_1->transform.position;
					glm::vec3 norm_ab = std::sqrt(std::pow(a_to_b.x, 2) + 
												  std::pow(a_to_b.y, 2) + 
												  std::pow(a_to_b.z, 2)) * a_to_b;
					//exchange velocities
					float temp = ball_1->transform.speed;
					ball_2->transform.speed = temp;
					ball_1->transform.speed = 0.0f;
					//ball_2->transform.velocity += 100.0f * ball_1->transform.speed * norm_ab;
				}
			};

			auto goal_collision = [&](Scene::Object *goal, Scene::Object *ball) {

				float distance = std::sqrt(std::pow(ball->transform.position.x - goal->transform.position.x, 2)
								 + std::pow(ball->transform.position.y - goal->transform.position.y, 2));
				if (distance <= (collision_radius + score_collision_radius)) {
					return true;
				}
				return false;
			};

			auto border_collision = [&](Scene::Object *object) {
				//check if object hit border
				if (object->transform.position.x > 2.86f)
					object->transform.velocity.x = -std::abs(object->transform.velocity.x);
				if (object->transform.position.x < -2.86f)
					object->transform.velocity.x = std::abs(object->transform.velocity.x);
				if (object->transform.position.y > 1.9f)
					object->transform.velocity.y = -std::abs(object->transform.velocity.y);
				if (object->transform.position.y < -1.9f)
					object->transform.velocity.y = std::abs(object->transform.velocity.y);
			};

			int index_delete = -1;

			for (uint32_t i = 0; i < ball_object_list.size(); i++) {
				for (uint32_t j = 0; j < dozer_object_list.size(); j++) {
					//Dozer Collisions with Ball
					dozer_collision(dozer_object_list[j], ball_object_list[i]);
					border_collision(dozer_object_list[j]);
					
				}
				for (uint32_t j = 0; j < cylinder_object_list.size(); j++) {
					if (goal_collision(cylinder_object_list[j], ball_object_list[i])) {
						index_delete = i;
					}
				}

				//Constantly move balls
				ball_object_list[i]->transform.position += (
					ball_object_list[i]->transform.speed * 
					ball_object_list[i]->transform.velocity);
				//Constantly apply friction to balls
				if (ball_object_list[i]->transform.speed <= 0.000001f)
					ball_object_list[i]->transform.speed = 0.0f; //set to stop
				else {
					ball_object_list[i]->transform.speed *= friction; //exponential decrease
					//constantly rotate balls
					ball_object_list[i]->transform.rotation = glm::angleAxis(
						ball_object_list[i]->transform.speed,
						ball_object_list[i]->transform.velocity
						);
				}
				//Constantly apply gravity
				if (ball_object_list[i]->transform.position.z >= (0.001f + collision_radius)) {
					ball_object_list[i]->transform.position.z -= gravity;
				}
				else {
					//ball hit the ground, bounce back if speed is high enough
					if (ball_object_list[i]->transform.speed >= 0.001f) {
						ball_object_list[i]->transform.position += glm::vec3(
							0.0f, 0.0f, 0.0001f);
					}
				}
			}


			//Ball collisions with Ball
			for (uint32_t i = 0; i < ball_object_list.size(); i++) {
				for (uint32_t j = 0; j < ball_object_list.size(); j++) {
					sphere_collision(ball_object_list[i], ball_object_list[j]);
				}
				//Ball collisions with sides
				for (uint32_t j = 0; j < cylinder_object_list.size(); j++) {
					if (goal_collision(cylinder_object_list[j], ball_object_list[i])) {
						index_delete = i;
					}
				}
				border_collision(ball_object_list[i]);
			}

			//Deletes ball if there is a collision with goal
			if (index_delete != -1) {
				// ball_object_list.erase(ball_object_list.begin() + (index_delete - 1));
				// ball_rotation.erase(ball_rotation.begin() + (index_delete - 1));
				index_delete = -1; //reset ball index
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

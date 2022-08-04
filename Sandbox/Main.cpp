// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <cstdio>
#include <iostream>
#include <fstream>
#include <AnKi/AnKi.h>

using namespace anki;

#define PLAYER 0
#define MOUSE 1

class MyApp : public App
{
public:
	Bool m_profile = false;
	ConfigSet m_config;

	Error init(int argc, char* argv[]);
	Error userMainLoop(Bool& quit, Second elapsedTime) override;
};

MyApp* app = nullptr;

Error MyApp::init(int argc, char* argv[])
{
	if(argc < 2)
	{
		ANKI_LOGE("usage: %s relative/path/to/scene.lua [anki config options]", argv[0]);
		return Error::USER_DATA;
	}

	// Config
	m_config.init(allocAligned, nullptr);
	ANKI_CHECK(m_config.setFromCommandLineArguments(argc - 2, argv + 2));

	// Init super class
	ANKI_CHECK(App::init(&m_config, allocAligned, nullptr));

	// Other init
	ResourceManager& resources = getResourceManager();

	if(getenv("PROFILE"))
	{
		m_profile = true;
		m_config.setCoreTargetFps(240);
		TracerSingleton::get().setEnabled(true);
	}

	// Load scene
	ScriptResourcePtr script;
	ANKI_CHECK(resources.loadResource(argv[1], script));
	ANKI_CHECK(getScriptManager().evalString(script->getSource()));

	// ANKI_CHECK(renderer.getFinalComposite().loadColorGradingTexture(
	//	"textures/color_gradient_luts/forge_lut.ankitex"));

#if PLAYER
	SceneGraph& scene = getSceneGraph();
	SceneNode& cam = scene.getActiveCameraNode();

	PlayerNode* pnode;
	ANKI_CHECK(scene.newSceneNode<PlayerNode>(
		"player", pnode, cam.getFirstComponentOfType<MoveComponent>().getLocalOrigin() - Vec4(0.0, 1.0, 0.0, 0.0)));

	cam.getFirstComponentOfType<MoveComponent>().setLocalTransform(
		Transform(Vec4(0.0, 0.0, 0.0, 0.0), Mat3x4::getIdentity(), 1.0));

	pnode->addChild(&cam);
#endif

	return Error::NONE;
}

Error MyApp::userMainLoop(Bool& quit, Second elapsedTime)
{
	quit = false;

	SceneGraph& scene = getSceneGraph();
	Input& in = getInput();
	Renderer& renderer = getMainRenderer().getOffscreenRenderer();

	if(in.getKey(KeyCode::ESCAPE))
	{
		quit = true;
		return Error::NONE;
	}

	// move the camera
	static MoveComponent* mover = &scene.getActiveCameraNode().getFirstComponentOfType<MoveComponent>();

	if(in.getKey(KeyCode::_1))
	{
		mover = scene.getActiveCameraNode().tryGetFirstComponentOfType<MoveComponent>();
	}
	if(in.getKey(KeyCode::_2))
	{
		mover = &scene.findSceneNode("Point.018_Orientation").getFirstComponentOfType<MoveComponent>();
	}

	if(in.getKey(KeyCode::L) == 1)
	{
		/*Vec3 origin = mover->getWorldTransform().getOrigin().xyz();
		printf("%f %f %f\n", origin.x(), origin.y(), origin.z());*/
		mover->setLocalOrigin(Vec4(81.169312f, -2.309618f, 17.088392f, 0.0f));
		// mover->setLocalRotation(Mat3x4::getIdentity());
	}

	if(in.getKey(KeyCode::F1) == 1)
	{
		static U mode = 0;
		mode = (mode + 1) % 3;
		if(mode == 0)
		{
			getConfig().setRDbgEnabled(false);
		}
		else if(mode == 1)
		{
			getConfig().setRDbgEnabled(true);
			renderer.getDbg().setDepthTestEnabled(true);
			renderer.getDbg().setDitheredDepthTestEnabled(false);
		}
		else
		{
			getConfig().setRDbgEnabled(true);
			renderer.getDbg().setDepthTestEnabled(false);
			renderer.getDbg().setDitheredDepthTestEnabled(true);
		}
	}
	if(in.getKey(KeyCode::F2) == 1)
	{
		// renderer.getDbg().flipFlags(DbgFlag::SPATIAL_COMPONENT);
	}
	if(in.getKey(KeyCode::F3) == 1)
	{
		// renderer.getDbg().flipFlags(DbgFlag::PHYSICS);
	}
	if(in.getKey(KeyCode::F4) == 1)
	{
		// renderer.getDbg().flipFlags(DbgFlag::SECTOR_COMPONENT);
	}
	if(in.getKey(KeyCode::F6) == 1)
	{
		renderer.getDbg().switchDepthTestEnabled();
	}

	if(in.getKey(KeyCode::F11) == 1)
	{
		TracerSingleton::get().setEnabled(!TracerSingleton::get().getEnabled());
	}

#if !PLAYER
	static Vec2 mousePosOn1stClick = in.getMousePosition();
	if(in.getMouseButton(MouseButton::RIGHT) == 1)
	{
		// Re-init mouse pos
		mousePosOn1stClick = in.getMousePosition();
	}

	if(in.getMouseButton(MouseButton::RIGHT) || in.hasTouchDevice())
	{
		constexpr F32 ROTATE_ANGLE = toRad(2.5f);
		constexpr F32 MOUSE_SENSITIVITY = 5.0f;

		in.hideCursor(true);

		if(in.getKey(KeyCode::_1) == 1)
		{
			mover = &scene.getActiveCameraNode().getFirstComponentOfType<MoveComponent>();
		}

		if(in.getKey(KeyCode::F1) == 1)
		{
			static U mode = 0;
			mode = (mode + 1) % 3;
			if(mode == 0)
			{
				getConfig().setRDbgEnabled(false);
			}
			else if(mode == 1)
			{
				getConfig().setRDbgEnabled(true);
				renderer.getDbg().setDepthTestEnabled(true);
				renderer.getDbg().setDitheredDepthTestEnabled(false);
			}
			else
			{
				getConfig().setRDbgEnabled(true);
				renderer.getDbg().setDepthTestEnabled(false);
				renderer.getDbg().setDitheredDepthTestEnabled(true);
			}
		}
		if(in.getKey(KeyCode::F2) == 1)
		{
			// renderer.getDbg().flipFlags(DbgFlag::SPATIAL_COMPONENT);
		}

		if(in.getKey(KeyCode::UP))
		{
			mover->rotateLocalX(ROTATE_ANGLE);
		}

		if(in.getKey(KeyCode::DOWN))
		{
			mover->rotateLocalX(-ROTATE_ANGLE);
		}

		if(in.getKey(KeyCode::LEFT))
		{
			mover->rotateLocalY(ROTATE_ANGLE);
		}

		if(in.getKey(KeyCode::RIGHT))
		{
			mover->rotateLocalY(-ROTATE_ANGLE);
		}

		static F32 moveDistance = 0.1f;
		if(in.getMouseButton(MouseButton::SCROLL_UP) == 1)
		{
			moveDistance += 0.1f;
			moveDistance = min(moveDistance, 10.0f);
		}

		if(in.getMouseButton(MouseButton::SCROLL_DOWN) == 1)
		{
			moveDistance -= 0.1f;
			moveDistance = max(moveDistance, 0.1f);
		}

		if(in.getKey(KeyCode::A))
		{
			mover->moveLocalX(-moveDistance);
		}

		if(in.getKey(KeyCode::D))
		{
			mover->moveLocalX(moveDistance);
		}

		if(in.getKey(KeyCode::Q))
		{
			mover->moveLocalY(-moveDistance);
		}

		if(in.getKey(KeyCode::E))
		{
			mover->moveLocalY(moveDistance);
		}

		if(in.getKey(KeyCode::W))
		{
			mover->moveLocalZ(-moveDistance);
		}

		if(in.getKey(KeyCode::S))
		{
			mover->moveLocalZ(moveDistance);
		}

		if(in.getKey(KeyCode::F12) == 1 && ANKI_ENABLE_TRACE)
		{
			TracerSingleton::get().setEnabled(!TracerSingleton::get().getEnabled());
		}

		const Vec2 velocity = in.getMousePosition() - mousePosOn1stClick;
		in.moveCursor(mousePosOn1stClick);
		if(velocity != Vec2(0.0))
		{
			Euler angles(mover->getLocalRotation().getRotationPart());
			angles.x() += velocity.y() * toRad(360.0f) * F32(elapsedTime) * MOUSE_SENSITIVITY;
			angles.x() = clamp(angles.x(), toRad(-90.0f), toRad(90.0f)); // Avoid cycle in Y axis
			angles.y() += -velocity.x() * toRad(360.0f) * F32(elapsedTime) * MOUSE_SENSITIVITY;
			angles.z() = 0.0f;
			mover->setLocalRotation(Mat3x4(Vec3(0.0f), angles));
		}

		static TouchPointer rotateCameraTouch = TouchPointer::COUNT;
		static Vec2 rotateEventInitialPos = Vec2(0.0f);
		for(TouchPointer touch : EnumIterable<TouchPointer>())
		{
			if(rotateCameraTouch == TouchPointer::COUNT && in.getTouchPointer(touch) == 1
			   && in.getTouchPointerNdcPosition(touch).x() > 0.1f)
			{
				rotateCameraTouch = touch;
				rotateEventInitialPos = in.getTouchPointerNdcPosition(touch) * getWindow().getAspectRatio();
				break;
			}
		}

		if(rotateCameraTouch != TouchPointer::COUNT && in.getTouchPointer(rotateCameraTouch) == 0)
		{
			rotateCameraTouch = TouchPointer::COUNT;
		}

		if(rotateCameraTouch != TouchPointer::COUNT && in.getTouchPointer(rotateCameraTouch) > 1)
		{
			Vec2 velocity =
				in.getTouchPointerNdcPosition(rotateCameraTouch) * getWindow().getAspectRatio() - rotateEventInitialPos;
			velocity *= 0.3f;

			Euler angles(mover->getLocalRotation().getRotationPart());
			angles.x() += velocity.y() * toRad(360.0f) * F32(elapsedTime) * MOUSE_SENSITIVITY;
			angles.x() = clamp(angles.x(), toRad(-90.0f), toRad(90.0f)); // Avoid cycle in Y axis
			angles.y() += -velocity.x() * toRad(360.0f) * F32(elapsedTime) * MOUSE_SENSITIVITY;
			angles.z() = 0.0f;
			mover->setLocalRotation(Mat3x4(Vec3(0.0f), angles));
		}

		static TouchPointer moveCameraTouch = TouchPointer::COUNT;
		static Vec2 moveEventInitialPos = Vec2(0.0f);
		for(TouchPointer touch : EnumIterable<TouchPointer>())
		{
			if(moveCameraTouch == TouchPointer::COUNT && in.getTouchPointer(touch) == 1
			   && in.getTouchPointerNdcPosition(touch).x() < -0.1f)
			{
				moveCameraTouch = touch;
				moveEventInitialPos = in.getTouchPointerNdcPosition(touch) * getWindow().getAspectRatio();
				break;
			}
		}

		if(moveCameraTouch != TouchPointer::COUNT && in.getTouchPointer(moveCameraTouch) == 0)
		{
			moveCameraTouch = TouchPointer::COUNT;
		}

		if(moveCameraTouch != TouchPointer::COUNT && in.getTouchPointer(moveCameraTouch) > 0)
		{
			Vec2 velocity =
				in.getTouchPointerNdcPosition(moveCameraTouch) * getWindow().getAspectRatio() - moveEventInitialPos;
			velocity *= 2.0f;

			mover->moveLocalX(moveDistance * velocity.x());
			mover->moveLocalZ(moveDistance * -velocity.y());
		}
	}
	else
	{
		in.hideCursor(false);
	}
#endif

	if(in.getKey(KeyCode::U) == 1)
	{
		renderer.setCurrentDebugRenderTarget(
			(renderer.getCurrentDebugRenderTarget() == "IndirectDiffuse") ? "" : "IndirectDiffuse");
	}

	if(in.getKey(KeyCode::I) == 1)
	{
		renderer.setCurrentDebugRenderTarget((renderer.getCurrentDebugRenderTarget() == "SSR") ? "" : "SSR");
	}

	if(in.getKey(KeyCode::O) == 1)
	{
		renderer.setCurrentDebugRenderTarget((renderer.getCurrentDebugRenderTarget() == "SM_resolve") ? ""
																									  : "SM_resolve");
	}

	if(in.getKey(KeyCode::H) == 1)
	{
		renderer.setCurrentDebugRenderTarget((renderer.getCurrentDebugRenderTarget() == "RtShadows") ? ""
																									 : "RtShadows");
	}

	/*if(in.getKey(KeyCode::J) == 1)
	{
		renderer.setCurrentDebugRenderTarget((renderer.getCurrentDebugRenderTarget() == "MotionVectorsHistoryLength")
												 ? ""
												 : "MotionVectorsHistoryLength");
	}*/

	if(in.getKey(KeyCode::P) == 1)
	{
		static U32 idx = 3;
		++idx;
		idx %= 4;
		if(idx == 0)
		{
			renderer.setCurrentDebugRenderTarget("IndirectDiffuseVrsSri");
		}
		else if(idx == 1)
		{
			renderer.setCurrentDebugRenderTarget("VrsSriDownscaled");
		}
		else if(idx == 2)
		{
			renderer.setCurrentDebugRenderTarget("VrsSri");
		}
		else
		{
			renderer.setCurrentDebugRenderTarget("");
		}
	}

	if(in.getKey(KeyCode::J) == 1)
	{
		m_config.setRVrs(!m_config.getRVrs());
	}

	if(in.getEvent(InputEvent::WINDOW_CLOSED))
	{
		quit = true;
	}

	if(m_profile && getGlobalTimestamp() == 1000)
	{
		quit = true;
		return Error::NONE;
	}

	return Error::NONE;
}

ANKI_MAIN_FUNCTION(myMain)
int myMain(int argc, char* argv[])
{
	Error err = Error::NONE;

	app = new MyApp;
	err = app->init(argc, argv);
	if(!err)
	{
		err = app->mainLoop();
	}

	if(err)
	{
		ANKI_LOGE("Error reported. See previous messages");
	}
	else
	{
		ANKI_LOGI("Bye!!");
	}
	delete app;

	return 0;
}

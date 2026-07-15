#include "engine.h"

#include <vsgXchange/all.h>

#include <iostream>

namespace
{

vsg::ref_ptr<vsg::Node> createTextureQuad(vsg::ref_ptr<vsg::Data> sourceData, vsg::ref_ptr<vsg::Options> options)
{
    auto builder = vsg::Builder::create();
    builder->options = options;

    vsg::StateInfo state;
    state.image = sourceData;
    state.lighting = false;

    vsg::GeometryInfo geom;
    geom.dx.set(static_cast<float>(sourceData->width()), 0.0f, 0.0f);
    geom.dy.set(0.0f, 0.0f, static_cast<float>(sourceData->height()));
    geom.dz.set(0.0f, -1.0f, 0.0f);

    return builder->createQuad(geom, state);
}

} // namespace

bool Engine::init(const vsg::Path& modelPath)
{
    try
    {
        constexpr double nearFarRatio = 0.001;
        constexpr double horizonMountainHeight = 0.0;
        constexpr double LODScale = 1.0;

        auto windowTraits = vsg::WindowTraits::create();

        options = vsg::Options::create();
        options->sharedObjects = vsg::SharedObjects::create();
        options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
        options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
        options->add(vsgXchange::all::create());

        auto object = vsg::read(modelPath, options);
        if (auto node = object.cast<vsg::Node>())
        {
            scene = node;
        }
        else if (auto data = object.cast<vsg::Data>())
        {
            scene = createTextureQuad(data, options);
        }
        else if (object)
        {
            std::cerr << "Unable to view object of type " << object->className() << std::endl;
            return false;
        }
        else
        {
            std::cerr << "Unable to load file " << modelPath << std::endl;
            return false;
        }

        if (!scene)
        {
            std::cerr << "Failed to create scene from " << modelPath << std::endl;
            return false;
        }

        viewer = vsg::Viewer::create();
        auto window = vsg::Window::create(windowTraits);
        if (!window)
        {
            std::cerr << "Could not create window." << std::endl;
            return false;
        }

        viewer->addWindow(window);

        auto ellipsoidModel = scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel");

        vsg::ref_ptr<vsg::LookAt> lookAt;
        vsg::ref_ptr<vsg::ProjectionMatrix> perspective;
        if (ellipsoidModel)
        {
            vsg::ComputeBounds computeBounds;
            scene->accept(computeBounds);

            double initialRadius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.5;
            double modelToEarthRatio = (initialRadius / ellipsoidModel->radiusEquator());

            if (modelToEarthRatio < 1.0)
            {
                vsg::dvec3 lla = ellipsoidModel->convertECEFToLatLongAltitude((computeBounds.bounds.min + computeBounds.bounds.max) * 0.5);

                auto worldToLocal = ellipsoidModel->computeWorldToLocalTransform(lla);
                auto localToWorld = ellipsoidModel->computeLocalToWorldTransform(lla);

                computeBounds.matrixStack.clear();
                computeBounds.matrixStack.push_back(worldToLocal);
                computeBounds.bounds.reset();
                scene->accept(computeBounds);

                auto bounds = computeBounds.bounds;
                vsg::dvec3 centre = (bounds.min + bounds.max) * 0.5;
                double radius = vsg::length(bounds.max - bounds.min) * 0.5;

                lookAt = vsg::LookAt::create(localToWorld * (centre + vsg::dvec3(0.0, 0.0, radius)), localToWorld * centre, vsg::dvec3(0.0, 1.0, 0.0) * worldToLocal);
            }
            else
            {
                lookAt = vsg::LookAt::create(vsg::dvec3(initialRadius * 2.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, 1.0));
            }

            perspective = vsg::EllipsoidPerspective::create(lookAt, ellipsoidModel, 30.0, static_cast<double>(window->extent2D().width) / static_cast<double>(window->extent2D().height), nearFarRatio, horizonMountainHeight);
        }
        else
        {
            vsg::ComputeBounds computeBounds;
            scene->accept(computeBounds);

            vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
            double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;

            lookAt = vsg::LookAt::create(centre + vsg::dvec3(0.0, -radius * 3.5, 0.0), centre, vsg::dvec3(0.0, 0.0, 1.0));
            perspective = vsg::Perspective::create(30.0, static_cast<double>(window->extent2D().width) / static_cast<double>(window->extent2D().height), nearFarRatio * radius, radius * 10.5);
        }

        auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(window->extent2D()));

        viewer->addEventHandler(vsg::CloseHandler::create(viewer));

        auto cameraAnimation = vsg::CameraAnimationHandler::create(camera, vsg::Path{}, options);
        viewer->addEventHandler(cameraAnimation);
        if (cameraAnimation->animation)
        {
            cameraAnimation->play();
        }

        viewer->addEventHandler(vsg::Trackball::create(camera, ellipsoidModel));

        auto view = vsg::View::create(camera);
        view->LODScale = LODScale;
        view->addChild(vsg::createHeadlight());
        view->addChild(scene);

        auto renderGraph = vsg::RenderGraph::create(window, view);
        auto commandGraph = vsg::CommandGraph::create(window, renderGraph);
        viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});

        viewer->compile();

        auto animationGroups = vsg::visit<vsg::FindAnimations>(scene).animationGroups;
        for (auto ag : animationGroups)
        {
            if (!ag->animations.empty()) viewer->animationManager->play(ag->animations.front());
        }

        viewer->start_point() = vsg::clock::now();
        return true;
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

void Engine::run()
{
    if (!viewer)
    {
        std::cerr << "Engine not initialized." << std::endl;
        return;
    }

    while (viewer->advanceToNextFrame())
    {
        viewer->handleEvents();
        viewer->update();
        viewer->recordAndSubmit();
        viewer->present();
    }
}

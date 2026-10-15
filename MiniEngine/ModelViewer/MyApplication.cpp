#include "GameCore.h"


//#define LEGACY_RENDERER

using namespace GameCore;


class MyApplication : public GameCore::IGameApp
{
public:

    MyApplication( void ) {}

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;

private:
};

CREATE_APPLICATION( MyApplication )

void MyApplication::Startup(void)
{
}

void MyApplication::Cleanup(void)
{
}

void MyApplication::Update(float deltaT)
{
}

void MyApplication::RenderScene(void)
{
}

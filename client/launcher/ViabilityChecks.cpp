#include "StdInc.h"
#include <wrl.h>

#include <d3d11.h>
#include <d3d11_1.h>

#include <shellapi.h>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

bool DXGICheck()
{
    ComPtr<ID3D11Device> device;
    
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);

    if (FAILED(hr))
    {
        trace("Couldn't create a D3D11 device - HRESULT %08x\n", hr);
        return true;
    }

    ComPtr<IDXGIDevice> dxgiOrig;
    hr = device.As(&dxgiOrig);

    if (FAILED(hr))
    {
        trace("Not an IDXGIDevice?\n");
        return true;
    }

    ComPtr<IDXGIDevice2> dxgiCheck;
    hr = dxgiOrig.As(&dxgiCheck);

    if (FAILED(hr))
    {
        const wchar_t* suggestion = L"The game will exit now.";

        if (!IsWindows7SP1OrGreater())
        {
            suggestion = L"Please install Windows 7 SP1 or greater, and try again.";
        }
        else if (!IsWindows8OrGreater())
        {
            suggestion = L"Please install the Platform Update for Windows 7, and try again.";
        }

        MessageBox(nullptr, va(L"DXGI 1.2 support is required to run " PRODUCT_NAME L". %s", suggestion), PRODUCT_NAME, MB_OK | MB_ICONSTOP);

        if (IsWindows7SP1OrGreater() && !IsWindows8OrGreater())
        {
            ShellExecute(nullptr, L"open", L"https://www.microsoft.com/en-us/download/details.aspx?id=36805", nullptr, nullptr, SW_SHOWNORMAL);
        }

        return false;
    }

    return true;
}

bool VerifyViability()
{
    if (!DXGICheck())
    {
        return false;
    }

    auto SetProcessMitigationPolicy = (decltype(&::SetProcessMitigationPolicy))GetProcAddress(GetModuleHandle(L"kernel32.dll"), "SetProcessMitigationPolicy");

    if (SetProcessMitigationPolicy)
    {
        PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY dp;
        dp.DisableExtensionPoints = true;

        SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &dp, sizeof(dp));
    }

    return true;
}
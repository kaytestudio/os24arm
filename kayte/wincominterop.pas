unit WinComInterop;

{$mode objfpc}{$H+}

interface

uses
  Windows,
  SysUtils,
  Classes,
  ComObj,
  ActiveXs;

// Windows API declarations not provided by the units above.

function ShowMessage(const Msg: string): Integer; stdcall; // Wrapper for MessageBox
  external user32 name 'MessageBoxA'; // Or MessageBoxW for Unicode

procedure OutputDebugString(const lpOutputString: PChar); stdcall;
  external kernel32 name 'OutputDebugStringA'; // Or OutputDebugStringW for Unicode

function GetForegroundWindow: HWND; stdcall;
  external user32 name 'GetForegroundWindow';


const
  // Replace these sample IDs with the server's registered IDs.
  IID_IMyCustomComInterface: TGUID = '{77A45F32-F8D3-4E40-9B79-D4A465D292E3}';
  CLSID_MyComAutomationClass: TGUID = '{12684EAF-B121-4B98-90A6-C9986C593F6F}';

type
  // IUnknown provides reference counting and interface queries.
  IMyCustomComInterface = interface(IUnknown)
  ['{YOUR-GUID-FOR-IMYCUSTOMCOMINTERFACE-HERE}'] // Interface GUID must match constant above
    function GetMessage: WideString; stdcall;
    procedure SetValue(const AValue: Integer); stdcall;
    function GetValue: Integer; stdcall;
  end;

  // IDispatch supports Automation clients such as VBScript.
  IMyAutomationInterface = interface(IDispatch)
  ['{YOUR-GUID-FOR-IMYAUTOMATIONINTERFACE-HERE}'] // Interface GUID
    function GetAutomatedMessage: OleVariant; safecall; // safecall for error handling
    procedure SetAutomatedValue(const AValue: Integer); safecall;
  end;


// Create an in-process or local-server COM object.
function CreateComObject(const ClassID, InterfaceID: TGUID): IUnknown;
var
  hr: HRESULT;
  pUnk: IUnknown;
begin
  Result := nil;
  // COM must be initialized on this thread before creating objects.
  hr := CoInitialize(nil);
  if SUCCEEDED(hr) or (hr = S_FALSE) then
  begin
    hr := CoCreateInstance(ClassID, nil, CLSCTX_INPROC_SERVER or CLSCTX_LOCAL_SERVER, InterfaceID, Pointer(pUnk));
    if SUCCEEDED(hr) then
    begin
      Result := pUnk;
    end
    else
    begin
      Writeln('Error creating COM object (HRESULT: ', IntToHex(hr, 8), ')');
    end;
  end
  else
  begin
    Writeln('Error initializing COM library (HRESULT: ', IntToHex(hr, 8), ')');
  end;
end;


// ActiveX embedding needs a client site, supplied by the host form.
(*
procedure EmbedActiveXControl(const AClassName: string; AParentHandle: HWND);
var
  OleControl: IOleControl;
  OleObject: IOleObject;
  ClassID: TGUID;
  hr: HRESULT;
begin
  // Convert the registered program ID to a class ID.
  hr := CLSIDFromProgID(PWideChar(StringToOleStr(AClassName)), ClassID);
  if FAILED(hr) then
  begin
    Writeln('Error: Could not get CLSID for ProgID: ', AClassName);
    Exit;
  end;

  OleControl := CreateComObject(ClassID, IOleControl) as IOleControl;
  if Assigned(OleControl) then
  begin
    OleObject := OleControl as IOleObject;
    if Assigned(OleObject) then
    begin
      // TODO: Provide an IOleClientSite from the host form.
      Writeln('ActiveX control created, but embedding requires more code.');
    end;
  end;
end;
*)

initialization
  // Initialize COM here if the application uses it on the main thread.
  // CoInitializeEx(nil, COINIT_APARTMENTTHREADED); // Preferred for UI threads
finalization
  // CoUninitialize; // Uncomment if CoInitialize was called without CoUninitialize per thread.
end.

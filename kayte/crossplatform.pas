unit CrossPlatform;

{$mode objfpc} // Recommended for cross-platform compatibility with FPC
{$H+} // Enable {$H+} for FPC to use ansistring/string compatibility

interface

uses
  SysUtils, Classes;

function GetUserHomePath: string;

procedure PrintArchitectureInfo;

procedure ProcessStrings;

procedure PrintCompilerInfo;

procedure LogDebugInfo(const S: string);

implementation

function GetUserHomePath: string;
begin
  // On Windows, the home directory is often stored in the HOMEPATH env var.
  {$IFDEF MSWINDOWS}
  Result := GetEnvironmentVariable('HOMEPATH');
  {$ELSE}
  // On Linux and macOS, it's typically in the HOME env var.
  Result := GetEnvironmentVariable('HOME');
  {$ENDIF}
end;

procedure PrintArchitectureInfo;
begin
  WriteLn('--- Architecture Information ---');
  {$IFDEF CPUX64}
  WriteLn('This is a 64-bit architecture.');
  WriteLn('Size of PtrInt: ' + IntToStr(SizeOf(PtrInt)) + ' bytes.');
  {$ELSE}
  WriteLn('This is a 32-bit architecture.');
  WriteLn('Size of Pointer: ' + IntToStr(SizeOf(Pointer)) + ' bytes.');
  {$ENDIF}
  WriteLn('');
end;

procedure ProcessStrings;
var
  S: string;
begin
  WriteLn('--- String Processing ---');
  S := 'Hello, world!';
  // Delphi's standard string is Unicode (AnsiString on older versions),
  // while FPC defaults to AnsiString.
  {$IFDEF DELPHI}
  // WideString keeps this explicitly UTF-16 on Delphi.
  WriteLn('Delphi-specific string processing (Unicode assumed): ' + S);
  {$ENDIF}
  {$IFDEF FPC}
  // FPC has different functions for string handling depending on the version and mode.
  WriteLn('FPC-specific string processing (AnsiString assumed): ' + S);
  {$ENDIF}
  WriteLn('');
end;

procedure PrintCompilerInfo;
begin
  WriteLn('--- Compiler Information ---');
  {$IFDEF FPC}
  WriteLn('This code is being compiled by Free Pascal.');
  {$ELSEIF DEFINED(DELPHI)} // Use DEFINED(DELPHI) for modern Delphi
  WriteLn('This code is being compiled by Delphi.');
  {$ELSE}
  WriteLn('This code is being compiled by an unknown compiler.');
  {$ENDIF}
  WriteLn('');
end;

procedure LogDebugInfo(const S: string);
begin
  WriteLn('--- Debug Logging ---');
  // Select the logging format for the current compiler and platform.
  {$IFDEF FPC}
    {$IFDEF MSWINDOWS}
    // FPC on Windows-specific code
    WriteLn('FPC Windows Log: ' + S);
    {$ELSE}
    // FPC on other OSes
    WriteLn('FPC Log: ' + S);
    {$ENDIF}
  {$ELSEIF DEFINED(DELPHI)}
    {$IFDEF MSWINDOWS}
    // Delphi on Windows-specific code
    OutputDebugString(PChar(S)); // Uses a Windows API function
    {$ELSE}
    // Delphi on other OSes (e.g., macOS, Linux)
    WriteLn('Delphi Log: ' + S);
    {$ENDIF}
  {$ENDIF}
  WriteLn('');
end;

end.


Program kayte;
(*
* Programming language interpreter for Kreatyve Designs
* usage, with this tool you can make custom scripts
* to run on our own games, delivered by ringsce store
*)
{$mode objfpc}{$H+}
uses
  SysUtils, Classes, Zipper, fphttpclient, fpjson, jsonparser, Process, CLI,
  Bytecode, TestBytecode, VirtualMachine, XMLParser, SimpleHTTPServer, sdk;
  (*KayteToSNES*)

type
  //TInstruction = (NOP, LOAD, ADD, SUB, HALT);
  TInstruction = (NOP, LOAD, ADD, SUB, HALT, IRC_HELP, IRC_WHOIS, IRC_SERVER, IRC_CONNECT, IF_COND, ELSE_COND, ENDIF, CASE_COND, ENDCASE);



TVirtualMachine = class
  private
    FMemory: array of Byte;
    FRegisters: array of Integer;
    FPC: Integer; // Program Counter
    FRunning: Boolean;
    procedure InitializeMemory(Size: Integer);
    procedure InitializeRegisters(Count: Integer);
    procedure ExecuteInstruction(Instruction: TInstruction);
  public
    procedure Init(MemorySize: Integer; RegisterCount: Integer);
    procedure Run;
  end;

  (* Download Repo *)
  procedure DownloadMapsFromGitHubRepo(const RepoURL: string);
var
  Process: TProcess;
begin
  Process := TProcess.Create(nil);
  try
    Process.Executable := '/usr/bin/curl'; // Path to the curl executable
    Process.Parameters.Add('-LOk'); // -LOk flags to download files and follow redirects
    Process.Parameters.Add(RepoURL); // GitHub repository URL
    Process.Options := Process.Options + [poWaitOnExit];
    Process.Execute;
    Writeln('Maps downloaded successfully from GitHub repository.');
  finally
    Process.Free;
  end;
end;

(* Check for Updates *)
procedure CheckForUpdates(const URL: string);
var
  Process: TProcess;
  Response: TStringList;
begin
  Process := TProcess.Create(nil);
  Response := TStringList.Create;
  try
    Process.Executable := '/usr/bin/curl'; // Path to the curl executable
    Process.Parameters.Add('-sI'); // -s silent mode to suppress progress meter and error messages, -I fetches headers only
    Process.Parameters.Add(URL); // Resource URL
    Process.Options := Process.Options + [poUsePipes, poWaitOnExit];
    Process.Execute;
    Response.LoadFromStream(Process.Output);
    Writeln('Last-Modified:', Response.Values['Last-Modified']);
  finally
    Process.Free;
    Response.Free;
  end;
end;

  (* VM init *)

procedure TVirtualMachine.InitializeMemory(Size: Integer);
begin
  if Size > 0 then
  begin
    SetLength(FMemory, Size);
    FillChar(FMemory[0], Size, 0);
    Writeln('Memory initialized to ', Size, ' bytes.');
  end
  else
  begin
    Writeln('Error: Memory size must be greater than 0.');
  end;
end;

procedure TVirtualMachine.InitializeRegisters(Count: Integer);
begin
  if Count > 0 then
  begin
    SetLength(FRegisters, Count);
    FillChar(FRegisters[0], Count * SizeOf(Integer), 0);
    Writeln('Registers initialized to ', Count, '.');
  end
  else
  begin
    Writeln('Error: Register count must be greater than 0.');
  end;
end;

procedure TVirtualMachine.ExecuteInstruction(Instruction: TInstruction);
begin
  case Instruction of
    NOP: Writeln('Executing NOP (No Operation)');
    LOAD: Writeln('Executing LOAD');
    ADD: Writeln('Executing ADD');
    SUB: Writeln('Executing SUB');
    HALT:
    begin
      Writeln('Executing HALT');
      FRunning := False;
    end;
  else
    Writeln('Unknown instruction');
  end;
end;

(* Procedure to save .kayte files *)
procedure SaveKayteFileToBytecode(const SourceFile, OutputFile: string);
var
  BytecodeGen: TBytecodeGenerator;
begin
  BytecodeGen := TBytecodeGenerator.Create;
  try
    Writeln('Converting ', SourceFile, ' to bytecode...');
    BytecodeGen.GenerateBytecode(SourceFile, OutputFile);
    Writeln('Bytecode saved to ', OutputFile);
  finally
    BytecodeGen.Free;
  end;
end;


procedure TVirtualMachine.Init(MemorySize: Integer; RegisterCount: Integer);
begin
  if (MemorySize > 0) and (RegisterCount > 0) then
  begin
    InitializeMemory(MemorySize);
    InitializeRegisters(RegisterCount);
    FPC := 0;
    FRunning := True;
    Writeln('Virtual machine initialized with ', MemorySize, ' bytes of memory and ', RegisterCount, ' registers.');
  end
  else
  begin
    Writeln('Error: Both memory size and register count must be greater than 0.');
  end;
end;

procedure TVirtualMachine.Run;
begin
  if (FPC >= 0) and (FPC < Length(FMemory)) then
  begin
    // Main execution loop using repeat-until (do-while equivalent)
    repeat
      // Simulating fetching an instruction and executing it
      // Here we just cycle through some sample instructions for demonstration
      case FPC of
        0: ExecuteInstruction(NOP);
        1: ExecuteInstruction(LOAD);
        2: ExecuteInstruction(ADD);
        3: ExecuteInstruction(SUB);
        4: ExecuteInstruction(HALT);
      else
        ExecuteInstruction(HALT); // Default to HALT if out of range
      end;

      Inc(FPC);
    until not FRunning;
  end
  else
  begin
    Writeln('Error: Program counter out of range.');
  end;
end;

procedure CreatePK3File(SourceFile, TargetFile: string);
var
  Z: TZipper;
begin
  Z := TZipper.Create;
  try
    Z.FileName := TargetFile;
    Z.Entries.AddFileEntry(SourceFile);
    Z.ZipAllFiles;
    Writeln('Created PK3 file: ', TargetFile);
  finally
    Z.Free;
  end;
end;

var
  VM: TVirtualMachine;

var
  CLIHandler: TCLIHandler;
  BytecodeGen: TBytecodeGenerator;
  DTDParser: TDTDParser;
  Server: TSimpleHTTPServer;
  Port: Integer;
  I: Integer;
  RunDemo: Boolean;

begin
  // '--demo' opts into the standalone showcase below (DTD parser, ad-hoc
  // HTTP server, VM demo, map download, PK3 packaging) instead of normal
  // CLI usage. Checked directly here, before CLIHandler ever sees argv, so
  // it can't be misread as a positional filename by ParseArgs.
  RunDemo := False;
  for I := 1 to ParamCount do
    if ParamStr(I) = '--demo' then
      RunDemo := True;

  if not RunDemo then
  begin
    // Normal CLI usage: --help, --version, --compile, --run, --http, ...
    CLIHandler := TCLIHandler.Create('kc', '1.0.0');
    try
      CLIHandler.ParseArgs;
      CLIHandler.Execute;
    finally
      CLIHandler.Free;
    end;
    Exit;
  end;

  // The rest is a standalone showcase, not real CLI functionality - it
  // references placeholder assets (assets/ui/ui.dtd, example.com, a
  // template GitHub repo URL) that don't exist in this project.
  DTDParser := TDTDParser.Create('assets/ui/ui.dtd');
  try
    DTDParser.ParseDTD;
  finally
    DTDParser.Free;
  end;

  // Save Kayte source file to bytecode
  //SourceFile := 'example.kyte';   // This can be passed via CLI or hardcoded for now
  //OutputFile := 'example.bytecode'; // The generated bytecode file
  //SaveKayteFileToBytecode(SourceFile, OutputFile);


  try
    // Define the port number on which the server will run
    Port := 8080;  // You can change this port number if needed

    // Create the HTTP server
    Server := TSimpleHTTPServer.Create(Port);

    // Start the server
    Writeln('Starting Kings server on port ', Port);
    Server.StartServer;

    // Keep the server running until manually stopped
    Writeln('Server is running. Press [Ctrl+C] to stop...');
    while True do
      Sleep(1000);  // Keep the main thread alive

  except
    on E: Exception do
    begin
      Writeln('An error occurred: ', E.Message);
      Server.StopServer;
      //Halt(1);  // Exit with error code
    end;
  end;

  // Initialize the virtual machine
  VM := TVirtualMachine.Create;
  try
    (* Download Maps *)
    DownloadMapsFromGitHubRepo('https://github.com/yourusername/your-repo/maps.zip');

    (* Check for Updates *)
    CheckForUpdates('https://example.com/version.txt');

    // Initialize the virtual machine with 1024 bytes of memory and 16 registers
    VM.Init(1024, 16);
    VM.Run;

    // Create a PK3 file from the VM source code
    CreatePK3File('kings.lpr', 'vm.pk3');
  finally
    VM.Free;
  end;
end.


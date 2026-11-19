unit VirtualMachine;

{$mode objfpc}{$H+}

interface

uses
  SysUtils, Classes,   BytecodeTypes; // BytecodeTypes unit is essential for TByteCodeProgram

type
  TVirtualMachine = class(TObject)
   private
     FProgram: TByteCodeProgram;
     FInstructionPointer: LongInt; // To track the current instruction
   public
     // Constructor to accept the pre-loaded TByteCodeProgram object
     constructor Create(AProgram: TByteCodeProgram);
     destructor Destroy; override;
     procedure Run;
   end;

implementation

{ TVirtualMachine }

constructor TVirtualMachine.Create(AProgram: TByteCodeProgram);
begin
  inherited Create;
  FProgram := AProgram;
  FInstructionPointer := 0;

  // TCLIHandler.RunBytecodeFile owns and frees AProgram.
end;

destructor TVirtualMachine.Destroy;
begin
  // No need to free FProgram here, as the CLIHandler manages its lifetime.
  inherited Destroy;
end;

procedure TVirtualMachine.Run;
var
  InstructionCount: LongInt;
begin
  InstructionCount := Length(FProgram.Instructions);

  Writeln('Executing bytecode for program: ', FProgram.ProgramTitle);
  Writeln('Total instructions to execute: ', InstructionCount);

  if InstructionCount = 0 then
  begin
    Writeln('Program is empty. Execution finished.');
    Exit;
  end;

  while FInstructionPointer < InstructionCount do
  begin
    // TODO: Execute this instruction; currently the VM only advances the pointer.
    Writeln(Format('  [VM] Executing Instruction at IP %d...', [FInstructionPointer]));

    Inc(FInstructionPointer);

    if FInstructionPointer >= 10 then // stop after 10 until there's a halt instruction
    begin
       Writeln('  ... (Simulating remaining execution) ...');
       FInstructionPointer := InstructionCount; // End simulation
    end;
  end;

  Writeln('Execution finished successfully.');
end;

end.

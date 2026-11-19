unit InterpreterCore;

{$mode objfpc}{$H+}

interface

uses
  SysUtils, Classes,
  StrUtils,
  Contnrs,
  fgl,
  BytecodeTypes,
  Lexer,
  Parser,
  TokenDefs;

// Interpreter state is global, so separate interpreter instances cannot run concurrently.
var
  Code: TStringList;
  Labels: TStringList;
  Vars: TStringList;
  Subs: specialize TFPGMap<String, Integer>;
  Stack: TStack;
  pc: Integer;
  MyLexer: TLexer;
  MyParser: TParser;

type
  TVarType = (vtInteger, vtString);

  TVariable = record
    vtype: TVarType;
    intValue: Integer;
    strValue: String;
  end;

  PVariable = ^TVariable;

procedure SetVar(const name: String; val: TVariable);
function GetVar(const name: String): TVariable;

procedure ExecuteLine(const ALine: String; var APC: Integer);

implementation

procedure SetVar(const name: String; val: TVariable);
var
  idx: Integer;
  varPtr: PVariable;
begin
  idx := Vars.IndexOf(name);
  if idx = -1 then
  begin
    New(varPtr);
    varPtr^ := val;
    Vars.AddObject(name, TObject(varPtr));
  end
  else
  begin
    varPtr := PVariable(Vars.Objects[idx]);
    varPtr^ := val;
  end;
end;

function GetVar(const name: String): TVariable;
var
  idx: Integer;
  varPtr: PVariable;
begin
  idx := Vars.IndexOf(name);
  if idx = -1 then
  begin
    WriteLn('Error: Variable ', name, ' not defined');
    Halt(1);
  end;
  varPtr := PVariable(Vars.Objects[idx]);
  Result := varPtr^;
end;


procedure ExecuteLine(const ALine: String; var APC: Integer);
var
  CurrentToken: TToken;
  StatementType: String;
begin
  WriteLn(Format('Executing line %d: "%s"', [APC + 1, ALine]));

  // This pass classifies statements by prefix but does not parse or execute them.
  StatementType := AnsiUpperCase(Trim(ALine));

  if StartsText('PRINT', StatementType) then
  begin
    WriteLn('  -> Interpreting PRINT statement.');
  end
  else if StartsText('DIM', StatementType) then
  begin
    WriteLn('  -> Interpreting DIM statement.');
  end
  else if StartsText('LET', StatementType) or (Pos('=', StatementType) > 0) then
  begin
    WriteLn('  -> Interpreting Assignment statement.');
  end
  else if StartsText('GOTO', StatementType) then
  begin
    WriteLn('  -> Interpreting GOTO statement.');
  end
  else if StartsText('INPUT', StatementType) then
  begin
    WriteLn('  -> Interpreting INPUT statement.');
  end
  else if StartsText('SUB', StatementType) then
  begin
    WriteLn('  -> Interpreting SUB definition (skipping for now).');
  end
  else if StartsText('END', StatementType) then
  begin
    WriteLn('  -> Interpreting END statement. Halting.');
    Halt(0);
  end
  else
  begin
    WriteLn('  -> Unknown statement or empty line.');
  end;

end;

end.


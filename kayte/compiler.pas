unit Compiler;

{$mode objfpc}{$H+}

interface

uses
  SysUtils, Classes, Lexer, Parser, Assembler, VirtualMachine,
  TokenDefs, BytecodeTypes;

type
  TCompiler = class
  private
    FSourceCode: TStringList;
    FCommentFreeSource: TStringList;
    FCompilerSuccess: Boolean;

    function StripComments(const ASource: TStringList): TStringList;
  public
    constructor Create;
    destructor Destroy; override;
    function Compile(const ASourceFile: string): Boolean;
    function CompileSource(const ASource: TStringList): TByteCodeProgram;

  end;

implementation

{ TCompiler }

function TCompiler.StripComments(const ASource: TStringList): TStringList;
var
  S: string;
  ResultList: TStringList;
  I: Integer;
begin
  ResultList := TStringList.Create;
  for I := 0 to ASource.Count - 1 do
  begin
    S := ASource[I];
    if Pos('//', S) > 0 then
    begin
      S := Copy(S, 1, Pos('//', S) - 1);
    end;
    if Trim(S) <> '' then
    begin
      ResultList.Add(S);
    end;
  end;
  Result := ResultList;
end;

constructor TCompiler.Create;
begin
  inherited Create;
  FSourceCode := TStringList.Create;
  FCommentFreeSource := TStringList.Create;
  FCompilerSuccess := False;
end;

destructor TCompiler.Destroy;
begin
  FSourceCode.Free;
  FCommentFreeSource.Free;
  inherited Destroy;
end;

function TCompiler.Compile(const ASourceFile: string): Boolean;
var
  Lexer: TLexer;
  Parser: TParser;
  ByteCodeProg: TByteCodeProgram;
begin
  Result := False;
  FCompilerSuccess := False;

  if not FileExists(ASourceFile) then
  begin
    Writeln('Error: Input file not found: ', ASourceFile);
    Exit;
  end;

  FSourceCode.LoadFromFile(ASourceFile);
  FCommentFreeSource.Assign(StripComments(FSourceCode));

  Writeln('Compiling ', ASourceFile, '...');

  Lexer := TLexer.Create(FCommentFreeSource);
  try
    Parser := TParser.Create(Lexer);
    try
      ByteCodeProg := Parser.Parse;
      FCompilerSuccess := True;
      Result := True;
    finally
      Parser.Free;
    end;
  finally
    Lexer.Free;
  end;
end;

function TCompiler.CompileSource(const ASource: TStringList): TByteCodeProgram;
var
  Lexer: TLexer;
  Parser: TParser;
begin
  FCommentFreeSource.Assign(StripComments(ASource));

  Lexer := TLexer.Create(FCommentFreeSource);
  try
    Parser := TParser.Create(Lexer);
    try
      Result := Parser.Parse;
    finally
      Parser.Free;
    end;
  finally
    Lexer.Free;
  end;
end;

end.

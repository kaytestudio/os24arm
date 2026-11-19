unit UVM;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Forms, StdCtrls;

type
  IInterpreterCallback = interface
    ['{CAE57563-0966-441C-AE68-DBE489DB3467}']
    procedure Log(const Msg: String);
  end;

  TVM = class
  private
    FCallback: IInterpreterCallback;

  public
    destructor Destroy; override;
    constructor Create(ACallback: IInterpreterCallback);
    procedure ExecuteInterpretedFunction(const AFunctionName: String);
  end;

  // Minimal parser types used until the VM shares Kayte's parser units.
  TLexer = class;
  TTokenType = (ttUnknown, ttIdentifier, ttString, ttInteger, ttKeyword);
  TToken = record
    TokenType: TTokenType;
    Value: String;
    Line: Integer;
    Column: Integer;
  end;
  TBCValue = class;

  TParser = class
  private
    FLexer: TLexer;
    FCurrentToken: TToken;

    procedure ConsumeToken(ExpectedType: TTokenType);
    function PeekToken: TToken;

    procedure ParsePrintStatement;
    procedure ParseShowStatement;
    procedure ParseLetStatement(const VarName: String);
    procedure ParseIfStatement;

    function ParseExpression: TBCValue;
    function ParseTerm: TBCValue;
    function ParseFactor: TBCValue;

  public
    constructor Create(ALexer: TLexer);
    destructor Destroy; override;
    procedure ParseProgram;
  end;

implementation

uses
  UKfrmRuntime;

{ TVM }

constructor TVM.Create(ACallback: IInterpreterCallback);
begin
  inherited Create;
  FCallback := ACallback;
  if Assigned(FCallback) then
    FCallback.Log('VM: Virtual Machine created and initialized.');
end;

destructor TVM.Destroy;
begin
  if Assigned(FCallback) then
    FCallback.Log('VM: Virtual Machine destroyed.');
  FCallback := nil;
  inherited Destroy;
end;

procedure TVM.ExecuteInterpretedFunction(const AFunctionName: String);
var
  LoadedLoginForm: TForm;
  UsernameEdit: TEdit;
  PasswordEdit: TEdit;
begin
  if Assigned(FCallback) then
    FCallback.Log(SysUtils.Format('VM: Attempting to execute interpreted function: %s', [AFunctionName]));

  // TODO: Route these handlers through an IFormRuntime callback.
  if AFunctionName = 'HandleLoginButton' then
  begin
    if Assigned(FCallback) then
      FCallback.Log('VM: Executing interpreted HandleLoginButton logic.');

    if Assigned(FCallback) then
        FCallback.Log('VM: (Simulating) Login logic for HandleLoginButton.');
  end
  else if AFunctionName = 'HandleCancelButton' then
  begin
    if Assigned(FCallback) then
      FCallback.Log('VM: Executing interpreted HandleCancelButton logic. Closing form.');

    if Assigned(FCallback) then
        FCallback.Log('VM: (Simulating) Closing form logic for HandleCancelButton.');
  end
  else
  begin
    if Assigned(FCallback) then
      FCallback.Log(SysUtils.Format('VM: Interpreted function "%s" not found or not handled by VM.', [AFunctionName]));
  end;
end;

{ TParser }

constructor TParser.Create(ALexer: TLexer);
begin
  inherited Create;
  FLexer := ALexer;
end;

destructor TParser.Destroy;
begin
  inherited Destroy;
end;

// TODO: Connect these methods to the VM's parser.
procedure TParser.ConsumeToken(ExpectedType: TTokenType);
begin
end;

function TParser.PeekToken: TToken;
begin
  Result.TokenType := ttUnknown;
  Result.Value := '';
end;

procedure TParser.ParsePrintStatement;
begin
end;

procedure TParser.ParseShowStatement;
begin
end;

procedure TParser.ParseLetStatement(const VarName: String);
begin
end;

procedure TParser.ParseIfStatement;
begin
end;

function TParser.ParseExpression: TBCValue;
begin
  Result := nil;
end;

function TParser.ParseTerm: TBCValue;
begin
  Result := nil;
end;

function TParser.ParseFactor: TBCValue;
begin
  Result := nil;
end;

procedure TParser.ParseProgram;
begin
end;

end.

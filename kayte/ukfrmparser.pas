{ UKfrmParser.pas – minimal stub -------------------------------------------------}
unit UKfrmParser;

{$mode objfpc}{$H+}

interface

uses
  SysUtils,
  UKfrmTypes;

type
  TKfrmParser = class
  public
    function ParseKfrmFile(const AFile: String): TKfrmFormDef;
  end;

implementation

function TKfrmParser.ParseKfrmFile(const AFile: String): TKfrmFormDef;
begin
  Result := TKfrmFormDef.Create;
  Result.Name    := ChangeFileExt(ExtractFileName(AFile), '');
  Result.Caption := Result.Name;
  Result.Width   := 400;
  Result.Height  := 300;
end;

end.


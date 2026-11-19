unit FormGenerator;

uses
  SysUtils, Classes,
  VMUI;

interface
procedure SaveVMFormDefinition(const FormDef: TVMFormDefinition; const FileName: String);
var
  SL: TStringList;
  Control: TVMControl;
  Prop: TVMControlProperty;
  i, j: Integer;
begin
  SL := TStringList.Create;
  try
    SL.Add(Format('[FORM:%s]', [FormDef.Name]));

    for i := 0 to FormDef.Controls.Count - 1 do
    begin
      Control := TVMControl(FormDef.Controls[i]);
      SL.Add(Format('[CONTROL:%s:%s]', [Control.Name, GetEnumName(TypeInfo(TVMControlType), Ord(Control.ControlType))]));
      for j := 0 to High(Control.Properties) do
      begin
        Prop := Control.Properties[j];
        SL.Add(Format('  %s=%s', [Prop.PropName, Prop.PropValue]));
      end;
    end;

    SL.SaveToFile(FileName);
  finally
    SL.Free;
  end;
end;


procedure GenerateExampleForm;
var
  Form1Def: TVMFormDefinition;
  Button1: TVMControl;
  Label1: TVMControl;
begin
  Form1Def := TVMFormDefinition.Create('Form1');

  Form1Def.Controls.Add(TVMControl.Create);
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).Name := 'Form1';
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).ControlType := vctForm;
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).AddProperty('Caption', '"My First VM Form"');
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).AddProperty('Width', '400');
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).AddProperty('Height', '300');
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).AddProperty('Top', '100');
  TVMControl(Form1Def.Controls[Form1Def.Controls.Count - 1]).AddProperty('Left', '100');


  Button1 := TVMControl.Create;
  Button1.Name := 'Button1';
  Button1.ControlType := vctButton;
  Button1.AddProperty('Caption', '"Click Me!"');
  Button1.AddProperty('Left', '50');
  Button1.AddProperty('Top', '50');
  Button1.AddProperty('Width', '100');
  Button1.AddProperty('Height', '25');
  Form1Def.Controls.Add(Button1);

  Label1 := TVMControl.Create;
  Label1.Name := 'Label1';
  Label1.ControlType := vctLabel;
  Label1.AddProperty('Caption', '"Initial Text"');
  Label1.AddProperty('Left', '50');
  Label1.AddProperty('Top', '100');
  Label1.AddProperty('Width', '200');
  Label1.AddProperty('Height', '20');
  Form1Def.Controls.Add(Label1);

  SaveVMFormDefinition(Form1Def, 'Form1.kfrm');

  Form1Def.Free;
end;
